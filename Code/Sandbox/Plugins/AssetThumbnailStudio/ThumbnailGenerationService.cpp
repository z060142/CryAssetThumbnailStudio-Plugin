// Copyright 2026
#include "StdAfx.h"
#include "ThumbnailGenerationService.h"
#include "ThumbnailJobQueue.h"
#include "ThumbnailStageRenderer.h"

#include <AssetSystem/Asset.h>
#include <AssetSystem/AssetManager.h>
#include <AssetSystem/AssetType.h>
#include <ILevelEditor.h>
#include <PathUtils.h>

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QImageReader>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QRegularExpression>
#include <QThread>

#include <algorithm>
#include <cstring>

namespace AssetThumbnailStudio
{
namespace
{

constexpr int kMaximumBatchSize = 8;
constexpr size_t kMaximumCompletedRequests = 32;

class CBusyReset final
{
public:
	explicit CBusyReset(bool& busy)
		: m_busy(busy)
	{
		m_busy = true;
	}

	~CBusyReset() { m_busy = false; }

private:
	bool& m_busy;
};

bool IsMainThread()
{
	QCoreApplication* const pApplication = QCoreApplication::instance();
	return pApplication && pApplication->thread() == QThread::currentThread();
}

bool IsSupportedAssetType(const CAsset* pAsset)
{
	if (!pAsset || !pAsset->GetType())
	{
		return false;
	}
	const char* const szTypeName = pAsset->GetType()->GetTypeName();
	return szTypeName
		&& (std::strcmp(szTypeName, "Mesh") == 0
			|| std::strcmp(szTypeName, "AnimatedMesh") == 0
			|| std::strcmp(szTypeName, "SkinnedMesh") == 0
			|| std::strcmp(szTypeName, "Skeleton") == 0
			|| std::strcmp(szTypeName, "Character") == 0
			|| std::strcmp(szTypeName, "Material") == 0);
}

bool IsSupportedSourceExtension(const char* szExtension)
{
	return szExtension
		&& (stricmp(szExtension, "cgf") == 0
			|| stricmp(szExtension, "cga") == 0
			|| stricmp(szExtension, "skin") == 0
			|| stricmp(szExtension, "chr") == 0
			|| stricmp(szExtension, "skel") == 0
			|| stricmp(szExtension, "cdf") == 0
			|| stricmp(szExtension, "mtl") == 0);
}

QJsonObject Diagnostic(const char* severity, const char* code, const char* message, const char* hint,
	int itemIndex = -1, const QString& path = QString())
{
	QJsonObject diagnostic;
	diagnostic.insert("severity", severity);
	diagnostic.insert("code", code);
	diagnostic.insert("message", message);
	diagnostic.insert("hint", hint);
	if (itemIndex >= 0)
	{
		diagnostic.insert("itemIndex", itemIndex);
	}
	if (!path.isEmpty())
	{
		diagnostic.insert("path", path);
	}
	return diagnostic;
}

void AddDiagnostic(QJsonArray& diagnostics, const char* severity, const char* code, const char* message,
	const char* hint, int itemIndex = -1, const QString& path = QString())
{
	diagnostics.append(Diagnostic(severity, code, message, hint, itemIndex, path));
}

QJsonObject SafetyReadback()
{
	IEditor* const pEditor = GetIEditor();
	ILevelEditor* const pLevelEditor = pEditor ? pEditor->GetLevelEditor() : nullptr;
	QJsonObject safety;
	safety.insert("mainThread", IsMainThread());
	safety.insert("documentReady", pEditor && pEditor->IsDocumentReady());
	safety.insert("isLevelLoaded", pLevelEditor && pLevelEditor->IsLevelLoaded());
	safety.insert("writesOnlyCanonicalThumbnailPaths", true);
	safety.insert("saveLevelCalled", false);
	safety.insert("assetMetadataSaveCalled", false);
	safety.insert("arbitraryCommandExecution", false);
	return safety;
}

SThumbnailGenerationResponse SerializeResponse(const QJsonObject& response)
{
	SThumbnailGenerationResponse serialized;
	serialized.ok = response.value("ok").toBool(false);
	serialized.json = QJsonDocument(response).toJson(QJsonDocument::Compact).toStdString();
	return serialized;
}

SThumbnailGenerationResponse RequestError(const char* code, const char* message, const char* hint,
	const QString& requestId = QString(), int requested = 0)
{
	QJsonArray diagnostics;
	AddDiagnostic(diagnostics, "error", code, message, hint);
	QJsonObject summary;
	summary.insert("requested", requested);
	summary.insert("succeeded", 0);
	summary.insert("failed", requested);
	summary.insert("warnings", 0);
	summary.insert("elapsedMs", 0);
	QJsonObject response;
	response.insert("ok", false);
	response.insert("protocol", "cryagent.asset_thumbnail_studio/1");
	response.insert("operation", "asset_thumbnail_studio.generate");
	response.insert("requestId", requestId);
	response.insert("replayed", false);
	response.insert("summary", summary);
	response.insert("items", QJsonArray());
	response.insert("safety", SafetyReadback());
	response.insert("diagnostics", diagnostics);
	return SerializeResponse(response);
}

std::string PayloadKey(const std::vector<std::string>& metadataPaths)
{
	QJsonArray paths;
	for (const std::string& path : metadataPaths)
	{
		paths.append(QString::fromUtf8(path.c_str(), static_cast<int>(path.size())));
	}
	return QJsonDocument(paths).toJson(QJsonDocument::Compact).toStdString();
}

bool IsContainedPath(const QString& rootPath, const QString& candidatePath)
{
	const QString cleanRoot = QDir::cleanPath(QFileInfo(rootPath).absoluteFilePath());
	const QString cleanCandidate = QDir::cleanPath(QFileInfo(candidatePath).absoluteFilePath());
	return cleanCandidate.compare(cleanRoot, Qt::CaseInsensitive) == 0
		|| cleanCandidate.startsWith(cleanRoot + QLatin1Char('/'), Qt::CaseInsensitive);
}

const char* ValidateMetadataPath(const QString& path)
{
	if (path.isEmpty())
	{
		return "ATS_REQUEST_INVALID";
	}
	if (QDir::isAbsolutePath(path) || path.startsWith(QLatin1Char('/')) || path.startsWith(QStringLiteral("//")))
	{
		return "ATS_PATH_ABSOLUTE_DENIED";
	}
	if (path.contains(QLatin1Char('\\')) || path.contains(QLatin1Char(':')))
	{
		return "ATS_PATH_TRAVERSAL_DENIED";
	}
	for (const QChar character : path)
	{
		if (character.unicode() < 0x20)
		{
			return "ATS_PATH_TRAVERSAL_DENIED";
		}
	}
	const QStringList segments = path.split(QLatin1Char('/'));
	for (const QString& segment : segments)
	{
		if (segment.isEmpty() || segment == QLatin1String(".") || segment == QLatin1String(".."))
		{
			return "ATS_PATH_TRAVERSAL_DENIED";
		}
	}
	if (!path.endsWith(QLatin1String(".cryasset"), Qt::CaseInsensitive))
	{
		return "ATS_METADATA_EXTENSION_REQUIRED";
	}
	const QString assetsRoot = QtUtil::ToQString(PathUtil::GetGameProjectAssetsPath());
	if (!IsContainedPath(assetsRoot, QDir(assetsRoot).absoluteFilePath(path)))
	{
		return "ATS_PATH_TRAVERSAL_DENIED";
	}
	return nullptr;
}

const char* DiagnosticMessage(const char* code)
{
	if (std::strcmp(code, "ATS_RENDERER_NOT_READY") == 0) return "The thumbnail renderer did not initialize.";
	if (std::strcmp(code, "ATS_SOURCE_LOAD_FAILED") == 0) return "The static mesh or character source could not be loaded.";
	if (std::strcmp(code, "ATS_MATERIAL_LOAD_FAILED") == 0) return "The material source could not be loaded.";
	if (std::strcmp(code, "ATS_MATERIAL_NO_PREVIEW") == 0) return "The material disables preview rendering with MTL_FLAG_NOPREVIEW.";
	if (std::strcmp(code, "ATS_CHARACTER_BIND_POSE_FAILED") == 0) return "The character bind pose could not be processed.";
	if (std::strcmp(code, "ATS_CHARACTER_RESOURCES_UNAVAILABLE") == 0) return "The character render meshes did not become resident.";
	if (std::strcmp(code, "ATS_SOURCE_BOUNDS_INVALID") == 0) return "The loaded source returned invalid bounds.";
	if (std::strcmp(code, "ATS_RENDER_FRAME_FAILED") == 0) return "A thumbnail render frame failed.";
	if (std::strcmp(code, "ATS_CAPTURE_FAILED") == 0) return "The 512x512 capture failed.";
	if (std::strcmp(code, "ATS_DOWNSCALE_FAILED") == 0) return "Mitchell downscaling failed.";
	if (std::strcmp(code, "ATS_TEMP_CLEANUP_FAILED") == 0) return "The temporary 512x512 PNG could not be removed.";
	return "Thumbnail generation failed.";
}

const char* DiagnosticHint(const char* code)
{
	if (std::strcmp(code, "ATS_MATERIAL_LOAD_FAILED") == 0) return "Verify that the catalog data file is a readable project-relative .mtl.";
	if (std::strcmp(code, "ATS_MATERIAL_NO_PREVIEW") == 0) return "Clear MTL_FLAG_NOPREVIEW in the material editor only if this material should have a thumbnail.";
	return "Inspect the Sandbox log and renderer readiness, then retry.";
}

QJsonObject RenderReadback(const SThumbnailRenderResult& result)
{
	QJsonObject streaming;
	streaming.insert("pollFrames", result.streaming.pollFrames);
	streaming.insert("busyFrames", result.streaming.busyFrames);
	streaming.insert("quietFrames", result.streaming.quietFrames);
	streaming.insert("elapsedMs", result.streaming.elapsedMs);
	streaming.insert("lastGlobalRequestCount", static_cast<double>(result.streaming.lastGlobalRequestCount));
	streaming.insert("lastLocalPendingMipCount", static_cast<double>(result.streaming.lastLocalPendingMipCount));
	streaming.insert("usedTextureCount", static_cast<double>(result.streaming.usedTextureCount));
	streaming.insert("streamableTextureCount", static_cast<double>(result.streaming.streamableTextureCount));
	streaming.insert("directPrecacheRequestCount", static_cast<double>(result.streaming.directPrecacheRequestCount));
	streaming.insert("largestModelStreamableTextureName", QtUtil::ToQString(result.streaming.largestModelStreamableTextureName));
	streaming.insert("largestModelStreamableTextureMinLoadedMip", result.streaming.largestModelStreamableTextureMinLoadedMip);
	streaming.insert("poolOverflowObserved", result.streaming.poolOverflowObserved);
	streaming.insert("poolOverflowTotallyObserved", result.streaming.poolOverflowTotallyObserved);
	streaming.insert("timedOut", result.streaming.timedOut);
	streaming.insert("qualityGateSatisfied", result.streaming.QualityGateSatisfied());

	QJsonObject pipeline;
	pipeline.insert("contextReady", result.pipeline.contextReady);
	pipeline.insert("sourceLoaded", result.pipeline.sourceLoaded);
	pipeline.insert("sourceKind", QtUtil::ToQString(result.pipeline.sourceKind));
	pipeline.insert("sourceExtension", QtUtil::ToQString(result.pipeline.sourceExtension));
	pipeline.insert("previewGeometryPath", QtUtil::ToQString(result.pipeline.previewGeometryPath));
	pipeline.insert("characterBindPoseProcessed", result.pipeline.characterBindPoseProcessed);
	pipeline.insert("characterRenderResourcesReady", result.pipeline.characterRenderResourcesReady);
	pipeline.insert("characterAabbConverged", result.pipeline.characterAabbConverged);
	pipeline.insert("characterRenderMeshBoundsUsed", result.pipeline.characterRenderMeshBoundsUsed);
	pipeline.insert("characterAabbSamples", result.pipeline.characterAabbSamples);
	pipeline.insert("characterAabbStableFrames", result.pipeline.characterAabbStableFrames);
	QJsonObject characterAabb;
	const bool characterAabbApplicable = result.pipeline.sourceKind == "character";
	characterAabb.insert("applicable", characterAabbApplicable);
	if (characterAabbApplicable)
	{
		characterAabb.insert("initialMin", QJsonArray({ result.pipeline.characterInitialAabb.min.x, result.pipeline.characterInitialAabb.min.y, result.pipeline.characterInitialAabb.min.z }));
		characterAabb.insert("initialMax", QJsonArray({ result.pipeline.characterInitialAabb.max.x, result.pipeline.characterInitialAabb.max.y, result.pipeline.characterInitialAabb.max.z }));
		characterAabb.insert("finalMin", QJsonArray({ result.pipeline.characterFinalAabb.min.x, result.pipeline.characterFinalAabb.min.y, result.pipeline.characterFinalAabb.min.z }));
		characterAabb.insert("finalMax", QJsonArray({ result.pipeline.characterFinalAabb.max.x, result.pipeline.characterFinalAabb.max.y, result.pipeline.characterFinalAabb.max.z }));
	}
	pipeline.insert("characterAabb", characterAabb);
	pipeline.insert("characterLoadFlags", result.pipeline.characterLoadFlags);
	pipeline.insert("capture512Succeeded", result.pipeline.capture512Succeeded);
	pipeline.insert("capture512Converged", result.pipeline.capture512Converged);
	pipeline.insert("captureAttempts", result.pipeline.captureAttempts);
	pipeline.insert("mitchellDownscaleSucceeded", result.pipeline.mitchellDownscaleSucceeded);
	pipeline.insert("temporaryFileRemoved", result.pipeline.temporaryFileRemoved);
	pipeline.insert("outputSize", result.pipeline.outputSize);
	pipeline.insert("skyDomeLoaded", result.pipeline.skyDomeLoaded);
	pipeline.insert("skyDomeRendered", result.pipeline.skyDomeRendered);
	pipeline.insert("skyDomeRadius", result.pipeline.skyDomeRadius);
	pipeline.insert("skyDomeExpanded", result.pipeline.skyDomeExpanded);
	pipeline.insert("skyDomeContainsCamera", result.pipeline.skyDomeContainsCamera);
	pipeline.insert("skyDomeCameraDistance", result.pipeline.skyDomeCameraDistance);
	pipeline.insert("stageScale", result.pipeline.stageScale);
	pipeline.insert("largeStageScaled", result.pipeline.largeStageScaled);
	pipeline.insert("skyMaterialLoaded", result.pipeline.skyMaterialLoaded);
	pipeline.insert("skyMaterialTwoSided", result.pipeline.skyMaterialTwoSided);
	pipeline.insert("materialBackgroundTexturePath", QtUtil::ToQString(result.pipeline.materialBackgroundTexturePath));
	pipeline.insert("materialBackgroundTextureLoaded", result.pipeline.materialBackgroundTextureLoaded);
	pipeline.insert("materialBackgroundApplied", result.pipeline.materialBackgroundApplied);
	pipeline.insert("groundRendered", result.pipeline.groundRendered);
	pipeline.insert("probeSpecularLoaded", result.pipeline.probeSpecularLoaded);
	pipeline.insert("probeDiffuseLoaded", result.pipeline.probeDiffuseLoaded);
	pipeline.insert("probeEnabled", result.pipeline.probeEnabled);
	pipeline.insert("graphicsPipeline", QtUtil::ToQString(result.pipeline.graphicsPipeline));
	pipeline.insert("shadowMapRequested", result.pipeline.shadowMapRequested);
	pipeline.insert("shadowsCVarValue", result.pipeline.shadowsCVarValue);
	pipeline.insert("renderShadowsPassEnabled", result.pipeline.renderShadowsPassEnabled);
	pipeline.insert("nativeShadowStagesRegistered", result.pipeline.nativeShadowStagesRegistered);
	pipeline.insert("environmentAmbientIntensity", result.pipeline.environmentAmbientIntensity);
	pipeline.insert("environmentProbeIntensity", result.pipeline.environmentProbeIntensity);
	pipeline.insert("shadowOutcome", QtUtil::ToQString(result.pipeline.shadowOutcome));

	QJsonObject render;
	render.insert("streaming", streaming);
	render.insert("pipeline", pipeline);
	return render;
}

bool ReadBackPng(const QString& absolutePath, QJsonObject& readback)
{
	QFile file(absolutePath);
	if (!file.exists() || !file.open(QIODevice::ReadOnly))
	{
		return false;
	}
	const QByteArray bytes = file.readAll();
	file.close();
	if (bytes.isEmpty())
	{
		return false;
	}
	QImageReader reader(absolutePath);
	const QSize size = reader.size();
	const QByteArray format = reader.format().toUpper();
	if (!reader.canRead() || size.width() != 256 || size.height() != 256 || format != QByteArrayLiteral("PNG"))
	{
		return false;
	}
	readback.insert("exists", true);
	readback.insert("bytes", static_cast<double>(bytes.size()));
	readback.insert("width", size.width());
	readback.insert("height", size.height());
	readback.insert("format", QString::fromLatin1(format));
	readback.insert("sha256", QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex()));
	return true;
}

} // namespace

CThumbnailGenerationService::CThumbnailGenerationService()
	: m_pUiQueue(new CThumbnailJobQueue())
{
}

CThumbnailGenerationService::~CThumbnailGenerationService() = default;

SThumbnailGenerationResponse CThumbnailGenerationService::GenerateCryAgentRequest(const std::string& rawJson)
{
	QJsonParseError parseError;
	const QJsonDocument document = QJsonDocument::fromJson(QByteArray::fromStdString(rawJson), &parseError);
	if (parseError.error != QJsonParseError::NoError || !document.isObject())
	{
		return RequestError("ATS_REQUEST_INVALID", "The request is not a valid JSON object.", "Send one JSON object matching the operation request schema.");
	}
	const QJsonObject object = document.object();
	for (QJsonObject::const_iterator it = object.constBegin(); it != object.constEnd(); ++it)
	{
		if (it.key() != QLatin1String("cmd") && it.key() != QLatin1String("requestId")
			&& it.key() != QLatin1String("metadataPaths") && it.key() != QLatin1String("token"))
		{
			return RequestError("ATS_REQUEST_INVALID", "The request contains an unsupported property.", "Remove properties not declared by the operation schema.");
		}
	}
	if (!object.value("cmd").isString() || object.value("cmd").toString() != QLatin1String("asset_thumbnail_studio.generate"))
	{
		return RequestError("ATS_REQUEST_INVALID", "The cmd field is missing or incorrect.", "Set cmd to asset_thumbnail_studio.generate.");
	}
	if (!object.value("requestId").isString())
	{
		return RequestError("ATS_REQUEST_ID_INVALID", "requestId must be a string.", "Use 1-64 ASCII letters, digits, dot, underscore, or hyphen.");
	}
	const QString requestId = object.value("requestId").toString();
	static const QRegularExpression requestIdPattern(QStringLiteral("^[A-Za-z0-9._-]{1,64}$"));
	if (!requestIdPattern.match(requestId).hasMatch())
	{
		return RequestError("ATS_REQUEST_ID_INVALID", "requestId has an invalid format.", "Use 1-64 ASCII letters, digits, dot, underscore, or hyphen.", requestId);
	}
	if (!object.value("metadataPaths").isArray())
	{
		return RequestError("ATS_REQUEST_INVALID", "metadataPaths must be an array.", "Pass 1-8 project-relative .cryasset paths.", requestId);
	}
	const QJsonArray jsonPaths = object.value("metadataPaths").toArray();
	if (jsonPaths.isEmpty())
	{
		return RequestError("ATS_BATCH_EMPTY", "metadataPaths is empty.", "Pass at least one project-relative .cryasset path.", requestId);
	}
	if (jsonPaths.size() > kMaximumBatchSize)
	{
		return RequestError("ATS_BATCH_TOO_LARGE", "The batch exceeds eight items.", "Split the request into batches of at most eight.", requestId, jsonPaths.size());
	}
	std::vector<std::string> metadataPaths;
	metadataPaths.reserve(jsonPaths.size());
	for (const QJsonValue& value : jsonPaths)
	{
		if (!value.isString() || value.toString().isEmpty())
		{
			return RequestError("ATS_REQUEST_INVALID", "Every metadataPaths item must be a non-empty string.", "Pass project-relative .cryasset paths only.", requestId, jsonPaths.size());
		}
		metadataPaths.push_back(value.toString().toUtf8().toStdString());
	}
	return Generate(requestId.toStdString(), metadataPaths);
}

SThumbnailGenerationResponse CThumbnailGenerationService::Generate(const std::string& requestId,
	const std::vector<std::string>& metadataPaths)
{
	const std::string payloadKey = PayloadKey(metadataPaths);
	for (const SCachedRequest& cached : m_completedRequests)
	{
		if (cached.requestId != requestId)
		{
			continue;
		}
		if (cached.payloadKey != payloadKey)
		{
			return RequestError("ATS_REQUEST_ID_CONFLICT", "requestId was already used for a different payload.",
				"Use the original payload or choose a new requestId.", QString::fromStdString(requestId), static_cast<int>(metadataPaths.size()));
		}
		QJsonParseError parseError;
		QJsonDocument replay = QJsonDocument::fromJson(QByteArray::fromStdString(cached.responseJson), &parseError);
		if (parseError.error == QJsonParseError::NoError && replay.isObject())
		{
			QJsonObject object = replay.object();
			object.insert("replayed", true);
			return SerializeResponse(object);
		}
	}
	if (m_busy || (m_pUiQueue && m_pUiQueue->IsGenerating()))
	{
		return RequestError("ATS_GENERATION_BUSY", "Thumbnail generation is already in progress.",
			"Wait for the active UI or agent request to finish, then retry with the same requestId.", QString::fromStdString(requestId), static_cast<int>(metadataPaths.size()));
	}
	if (!IsMainThread())
	{
		return RequestError("ATS_NOT_MAIN_THREAD", "The operation was not dispatched on the Qt main thread.",
			"Invoke the operation through CryAgentSDKHost main-update dispatch.", QString::fromStdString(requestId), static_cast<int>(metadataPaths.size()));
	}
	IEditor* const pEditor = GetIEditor();
	if (!pEditor)
	{
		return RequestError("ATS_EDITOR_UNAVAILABLE", "Sandbox editor services are unavailable.",
			"Run the command in Sandbox.", QString::fromStdString(requestId), static_cast<int>(metadataPaths.size()));
	}
	if (!pEditor->IsDocumentReady())
	{
		return RequestError("ATS_DOCUMENT_NOT_READY", "The Sandbox document is not ready.",
			"Wait for documentReady=true before generating thumbnails.", QString::fromStdString(requestId), static_cast<int>(metadataPaths.size()));
	}
	ILevelEditor* const pLevelEditor = pEditor->GetLevelEditor();
	if (!pLevelEditor || !pLevelEditor->IsLevelLoaded())
	{
		return RequestError("ATS_LEVEL_NOT_LOADED", "No level is loaded.",
			"Open a level and verify isLevelLoaded=true before generating thumbnails.", QString::fromStdString(requestId), static_cast<int>(metadataPaths.size()));
	}
	CAssetManager* const pAssetManager = pEditor->GetAssetManager();
	if (!pAssetManager)
	{
		return RequestError("ATS_ASSET_MANAGER_UNAVAILABLE", "The asset manager is unavailable.",
			"Wait for Sandbox asset services to initialize.", QString::fromStdString(requestId), static_cast<int>(metadataPaths.size()));
	}
	if (pAssetManager->IsScanning())
	{
		return RequestError("ATS_ASSET_SCAN_IN_PROGRESS", "The asset catalog is still scanning.",
			"Wait for the asset scan to finish, then retry with the same requestId.", QString::fromStdString(requestId), static_cast<int>(metadataPaths.size()));
	}

	CBusyReset busyReset(m_busy);
	QElapsedTimer requestTimer;
	requestTimer.start();
	CryLog("[AssetThumbnailStudio] cryagent_generate_begin requestId=%s items=%llu replayed=false",
		requestId.c_str(), static_cast<unsigned long long>(metadataPaths.size()));

	CThumbnailStageRenderer renderer;
	QJsonArray items;
	QJsonArray topDiagnostics;
	int succeeded = 0;
	int warnings = 0;
	const QString assetsRoot = QtUtil::ToQString(PathUtil::GetGameProjectAssetsPath());
	for (size_t index = 0; index < metadataPaths.size(); ++index)
	{
		QElapsedTimer itemTimer;
		itemTimer.start();
		const QString requestedPath = QString::fromUtf8(metadataPaths[index].c_str(), static_cast<int>(metadataPaths[index].size()));
		QJsonObject item;
		QJsonArray itemDiagnostics;
		item.insert("index", static_cast<int>(index));
		item.insert("requestedMetadataPath", requestedPath);
		item.insert("resolvedMetadataPath", QJsonValue(QJsonValue::Null));
		item.insert("assetType", QJsonValue(QJsonValue::Null));
		item.insert("sourceFile", QJsonValue(QJsonValue::Null));
		item.insert("thumbnailPath", QJsonValue(QJsonValue::Null));
		item.insert("ok", false);
		item.insert("render", QJsonValue(QJsonValue::Null));
		item.insert("fileReadback", QJsonValue(QJsonValue::Null));
		item.insert("thumbnailInvalidated", false);

		const char* const pathError = ValidateMetadataPath(requestedPath);
		if (pathError)
		{
			const bool extensionError = std::strcmp(pathError, "ATS_METADATA_EXTENSION_REQUIRED") == 0;
			AddDiagnostic(itemDiagnostics, "error", pathError,
				extensionError ? "The selector is not a .cryasset metadata path." : "The metadata path is unsafe or not project-relative.",
				extensionError ? "Pass the asset's project-relative .cryasset path." : "Use forward-slash project-relative paths without dot segments.",
				static_cast<int>(index), requestedPath);
		}
		else
		{
			CAsset* const pAsset = pAssetManager->FindAssetForMetadata(metadataPaths[index].c_str());
			if (!pAsset)
			{
				AddDiagnostic(itemDiagnostics, "error", "ATS_ASSET_NOT_FOUND", "No catalog asset matched the metadata path.",
					"Wait for asset scanning to finish and verify the Asset Browser metadata path.", static_cast<int>(index), requestedPath);
			}
			else
			{
				const QString resolvedPath = QtUtil::ToQString(pAsset->GetMetadataFile());
				item.insert("resolvedMetadataPath", resolvedPath);
				if (QDir::cleanPath(resolvedPath).compare(QDir::cleanPath(requestedPath), Qt::CaseInsensitive) != 0)
				{
					AddDiagnostic(itemDiagnostics, "error", "ATS_ASSET_ROUNDTRIP_MISMATCH", "The resolved asset metadata path did not match the selector.",
						"Use the exact project-relative metadata path shown by the Asset Browser.", static_cast<int>(index), requestedPath);
				}
				else if (!IsSupportedAssetType(pAsset))
				{
					AddDiagnostic(itemDiagnostics, "error", "ATS_ASSET_TYPE_UNSUPPORTED", "The asset type is not supported by Asset Thumbnail Studio.",
						"Select a supported mesh, character, or material asset.", static_cast<int>(index), requestedPath);
				}
				else if (pAsset->GetFilesCount() == 0 || !IsSupportedSourceExtension(PathUtil::GetExt(pAsset->GetFile(0).c_str())))
				{
					AddDiagnostic(itemDiagnostics, "error", "ATS_ASSET_DATA_FORMAT_UNSUPPORTED", "The asset's first data file is not a supported mesh, character, or material format.",
						"Use a catalog asset backed by .cgf, .cga, .skin, .chr/.skel, .cdf, or .mtl.", static_cast<int>(index), requestedPath);
				}
				else
				{
					const char* const szAssetType = pAsset->GetType()->GetTypeName();
					item.insert("assetType", QString::fromUtf8(szAssetType ? szAssetType : ""));
					item.insert("sourceFile", QtUtil::ToQString(pAsset->GetFile(0)));
					const QString thumbnailPath = QtUtil::ToQString(pAsset->GetThumbnailPath());
					item.insert("thumbnailPath", thumbnailPath);
					const QString absoluteOutputPath = QDir(assetsRoot).absoluteFilePath(thumbnailPath);
					if (!IsContainedPath(assetsRoot, absoluteOutputPath))
					{
						AddDiagnostic(itemDiagnostics, "error", "ATS_OUTPUT_PATH_ESCAPE", "The asset's canonical thumbnail path escapes the project assets root.",
							"Repair the asset metadata before retrying.", static_cast<int>(index), requestedPath);
					}
					else
					{
						const SThumbnailRenderResult renderResult = renderer.RenderAssetThumbnailDetailed(pAsset);
						item.insert("render", RenderReadback(renderResult));
						if (!renderResult.ok)
						{
							const char* const code = renderResult.failureCode.empty() ? "ATS_RENDER_FRAME_FAILED" : renderResult.failureCode.c_str();
							AddDiagnostic(itemDiagnostics, "error", code, DiagnosticMessage(code), DiagnosticHint(code),
								static_cast<int>(index), requestedPath);
						}
						else
						{
							if (!renderResult.pipeline.probeEnabled)
							{
								AddDiagnostic(itemDiagnostics, "warning", "ATS_ENV_PROBE_DISABLED",
									"The environment probe cubemaps did not load; the thumbnail was generated with the probe disabled.",
									"Verify the configured specular and diffuse cubemap paths.", static_cast<int>(index), requestedPath);
								++warnings;
							}
							if (renderResult.streaming.timedOut)
							{
								AddDiagnostic(itemDiagnostics, "warning", "ATS_STREAMING_TIMEOUT_CAPTURED", "The texture/resource/AABB capture gate timed out and the thumbnail was captured anyway.",
									"Do not treat this item as passing the high-mip or character-framing quality gate.", static_cast<int>(index), requestedPath);
								++warnings;
							}
							QJsonObject fileReadback;
							if (!ReadBackPng(absoluteOutputPath, fileReadback))
							{
								AddDiagnostic(itemDiagnostics, "error", "ATS_OUTPUT_READBACK_FAILED", "The generated file failed PNG readback validation.",
									"Verify the canonical thumbnail is a non-empty 256x256 PNG.", static_cast<int>(index), requestedPath);
							}
							else
							{
								item.insert("fileReadback", fileReadback);
								pAsset->InvalidateThumbnail();
								item.insert("thumbnailInvalidated", true);
								item.insert("ok", true);
								++succeeded;
							}
						}
					}
				}
			}
		}
		item.insert("elapsedMs", static_cast<double>(itemTimer.elapsed()));
		item.insert("diagnostics", itemDiagnostics);
		items.append(item);
	}

	const int requested = static_cast<int>(metadataPaths.size());
	const int failed = requested - succeeded;
	if (succeeded > 0 && failed > 0)
	{
		AddDiagnostic(topDiagnostics, "error", "ATS_PARTIAL_FAILURE", "The non-transactional batch completed with partial success.",
			"Inspect each item diagnostic; successful thumbnails were not rolled back.");
	}
	QJsonObject summary;
	summary.insert("requested", requested);
	summary.insert("succeeded", succeeded);
	summary.insert("failed", failed);
	summary.insert("warnings", warnings);
	summary.insert("elapsedMs", static_cast<double>(requestTimer.elapsed()));
	QJsonObject response;
	response.insert("ok", failed == 0);
	response.insert("protocol", "cryagent.asset_thumbnail_studio/1");
	response.insert("operation", "asset_thumbnail_studio.generate");
	response.insert("requestId", QString::fromStdString(requestId));
	response.insert("replayed", false);
	response.insert("summary", summary);
	response.insert("items", items);
	response.insert("safety", SafetyReadback());
	response.insert("diagnostics", topDiagnostics);
	const SThumbnailGenerationResponse serialized = SerializeResponse(response);
	CacheCompleted(requestId, payloadKey, serialized);
	CryLog("[AssetThumbnailStudio] cryagent_generate_end requestId=%s ok=%s succeeded=%d failed=%d warnings=%d elapsedMs=%lld",
		requestId.c_str(), serialized.ok ? "true" : "false", succeeded, failed, warnings, requestTimer.elapsed());
	return serialized;
}

void CThumbnailGenerationService::CacheCompleted(const std::string& requestId, const std::string& payloadKey,
	const SThumbnailGenerationResponse& response)
{
	if (m_completedRequests.size() == kMaximumCompletedRequests)
	{
		m_completedRequests.pop_front();
	}
	SCachedRequest cached;
	cached.requestId = requestId;
	cached.payloadKey = payloadKey;
	cached.responseJson = response.json;
	cached.ok = response.ok;
	m_completedRequests.push_back(cached);
}

void CThumbnailGenerationService::GenerateForUi(const std::vector<CAssetPtr>& assets)
{
	if (m_busy)
	{
		CryWarning(VALIDATOR_MODULE_EDITOR, VALIDATOR_WARNING,
			"[AssetThumbnailStudio] Thumbnail generation is already in progress; ignored the UI request.");
		return;
	}
	if (m_pUiQueue)
	{
		m_pUiQueue->Enqueue(assets);
	}
}

void CThumbnailGenerationService::ShutdownUiQueue()
{
	if (m_pUiQueue)
	{
		m_pUiQueue->Shutdown();
	}
}

} // namespace AssetThumbnailStudio
