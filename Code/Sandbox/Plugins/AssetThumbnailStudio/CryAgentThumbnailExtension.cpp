// Copyright 2026
#include "StdAfx.h"
#include "CryAgentThumbnailExtension.h"
#include "ThumbnailGenerationService.h"

#include <ICryAgentExtensionHost.h>

#include <cstring>

namespace AssetThumbnailStudio
{
namespace
{

constexpr const char* kExtensionId = "asset_thumbnail_studio";
constexpr const char* kProviderId = "extension.asset_thumbnail_studio";
constexpr const char* kCommand = "asset_thumbnail_studio.generate";
constexpr const char* kProtocol = "cryagent.asset_thumbnail_studio/1";

class CGenerateThumbnailHandler final : public CryAgentSDK::IOperationHandler
{
public:
	explicit CGenerateThumbnailHandler(CThumbnailGenerationService& service)
		: m_service(service)
	{
	}

	CryAgentSDK::SOperationResult Execute(const CryAgentSDK::SOperationRequest& request) const override
	{
		const SThumbnailGenerationResponse generated = m_service.GenerateCryAgentRequest(request.rawLine);
		CryAgentSDK::SOperationResult result;
		result.ok = generated.ok;
		result.protocol = kProtocol;
		result.fields["legacyJson"] = generated.json;
		return result;
	}

private:
	CThumbnailGenerationService& m_service;
};

} // namespace

CCryAgentThumbnailExtension::CCryAgentThumbnailExtension(CThumbnailGenerationService& service)
	: m_service(service)
{
}

CCryAgentThumbnailExtension::~CCryAgentThumbnailExtension()
{
	Detach();
}

bool CCryAgentThumbnailExtension::Attach()
{
	if (m_pHost)
	{
		return true;
	}
	if (m_registrationFailed)
	{
		return false;
	}
	if (!gEnv || !gEnv->pSystem || !gEnv->pSystem->GetIPluginManager())
	{
		if (!m_hostUnavailableLogged)
		{
			m_hostUnavailableLogged = true;
			CryWarning(VALIDATOR_MODULE_EDITOR, VALIDATOR_WARNING,
				"[AssetThumbnailStudio] ATS_EXTENSION_HOST_UNAVAILABLE: CryAgent plugin manager is unavailable; the UI remains enabled and registration will retry from editor idle.");
		}
		return false;
	}
	CryAgentSDK::ICryAgentExtensionHost* const pHost =
		gEnv->pSystem->GetIPluginManager()->QueryPlugin<CryAgentSDK::ICryAgentExtensionHost>();
	if (!pHost)
	{
		if (!m_hostUnavailableLogged)
		{
			m_hostUnavailableLogged = true;
			CryWarning(VALIDATOR_MODULE_EDITOR, VALIDATOR_WARNING,
				"[AssetThumbnailStudio] ATS_EXTENSION_HOST_UNAVAILABLE: CryAgentSDKHost is not available yet; the UI remains enabled and registration will retry from editor idle.");
		}
		return false;
	}
	char error[512] = {};
	if (!pHost->RegisterExtensionV1(*this, error, sizeof(error)))
	{
		const bool contextNotReady = std::strstr(error, "context is not ready") != nullptr;
		if (contextNotReady)
		{
			if (!m_hostUnavailableLogged)
			{
				m_hostUnavailableLogged = true;
				CryWarning(VALIDATOR_MODULE_EDITOR, VALIDATOR_WARNING,
					"[AssetThumbnailStudio] ATS_EXTENSION_HOST_UNAVAILABLE: CryAgentSDKHost context is not ready yet; registration will retry from editor idle.");
			}
			return false;
		}
		m_registrationFailed = true;
		CryWarning(VALIDATOR_MODULE_EDITOR, VALIDATOR_WARNING,
			"[AssetThumbnailStudio] ATS_EXTENSION_REGISTRATION_FAILED: %s", error[0] ? error : "host rejected the extension");
		return false;
	}
	m_pHost = pHost;
	CryLog("[AssetThumbnailStudio] cryagent_extension_registered id=%s provider=%s command=%s",
		kExtensionId, kProviderId, kCommand);
	return true;
}

void CCryAgentThumbnailExtension::Detach()
{
	if (!m_pHost)
	{
		return;
	}
	CryAgentSDK::ICryAgentExtensionHost* pCurrentHost = nullptr;
	if (gEnv && gEnv->pSystem && gEnv->pSystem->GetIPluginManager())
	{
		pCurrentHost = gEnv->pSystem->GetIPluginManager()->QueryPlugin<CryAgentSDK::ICryAgentExtensionHost>();
	}
	if (pCurrentHost == m_pHost && pCurrentHost->UnregisterExtensionV1(kExtensionId))
	{
		CryLog("[AssetThumbnailStudio] cryagent_extension_unregistered id=%s", kExtensionId);
	}
	m_pHost = nullptr;
}

CryAgentSDK::SExtensionInfoV1 CCryAgentThumbnailExtension::ExtensionInfo() const
{
	CryAgentSDK::SExtensionInfoV1 info;
	info.id = kExtensionId;
	info.name = "Asset Thumbnail Studio";
	info.version = "0.2.2";
	info.description = "Sandbox-only thumbnail generation command owned by AssetThumbnailStudio.dll.";
	return info;
}

bool CCryAgentThumbnailExtension::RegisterExtension(CryAgentSDK::IExtensionRegistrarV1& registrar,
	CryAgentSDK::ISdkContext&, std::string& error)
{
	CryAgentSDK::SProviderDescriptor provider;
	provider.id = kProviderId;
	provider.name = "Asset Thumbnail Studio Extension";
	provider.protocol = kProtocol;
	provider.status = "experimental";
	provider.description = "Generates canonical Sandbox asset thumbnails through the AssetThumbnailStudio renderer.";
	provider.capabilities.push_back(kCommand);
	provider.types.push_back("asset.thumbnail.png");
	provider.types.push_back("asset.mesh.cgf");
	provider.types.push_back("asset.character.cga");
	provider.types.push_back("asset.character.skin");
	provider.types.push_back("asset.character.skeleton");
	provider.types.push_back("asset.character.cdf");
	provider.types.push_back("asset.material.mtl");
	provider.constraints = {
		"sandbox_editor_only", "project_asset_catalog_only", "project_relative_cryasset_paths_only",
		"mesh_character_and_material_sources", "canonical_thumbnail_output_only", "requires_document_ready",
		"requires_loaded_level_for_test_validity", "synchronous_main_thread", "batch_max_8",
		"non_transactional_file_write", "extension_port_not_editor_port"
	};
	provider.engineBacked = true;
	provider.scriptingRuntime = false;
	provider.centralDispatcherLogic = false;
	if (!registrar.RegisterProvider(provider, error))
	{
		return false;
	}

	CryAgentSDK::SOperationRegistrationV1 registration;
	CryAgentSDK::SOperationDescriptor& operation = registration.descriptor;
	operation.name = kCommand;
	operation.providerId = kProviderId;
	operation.command = kCommand;
	operation.protocol = kProtocol;
	operation.status = "experimental";
	operation.description = "Generate canonical 256x256 PNG thumbnails for one to eight catalog mesh, character, or material assets.";
	operation.category = "asset";
	operation.tags = { "asset", "thumbnail", "sandbox", "extension", "render", "test-automation" };
	operation.constraints = {
		"curated_behavior_effect", "project_relative_cryasset_paths_only", "canonical_thumbnail_output_only",
		"mesh_character_and_material_sources", "requires_document_ready", "requires_loaded_level", "synchronous_main_thread",
		"batch_max_8", "partial_success_possible", "non_transactional", "no_level_save",
		"no_asset_metadata_save", "no_arbitrary_output_path", "streaming_timeout_may_capture_with_warning"
	};
	operation.diagnostics = {
		"ATS_REQUEST_INVALID", "ATS_REQUEST_ID_INVALID", "ATS_REQUEST_ID_CONFLICT", "ATS_BATCH_EMPTY",
		"ATS_BATCH_TOO_LARGE", "ATS_GENERATION_BUSY", "ATS_NOT_MAIN_THREAD", "ATS_EDITOR_UNAVAILABLE",
		"ATS_DOCUMENT_NOT_READY", "ATS_LEVEL_NOT_LOADED", "ATS_ASSET_MANAGER_UNAVAILABLE",
		"ATS_ASSET_SCAN_IN_PROGRESS", "ATS_PATH_ABSOLUTE_DENIED", "ATS_PATH_TRAVERSAL_DENIED",
		"ATS_METADATA_EXTENSION_REQUIRED", "ATS_ASSET_NOT_FOUND", "ATS_ASSET_ROUNDTRIP_MISMATCH",
		"ATS_ASSET_TYPE_UNSUPPORTED", "ATS_ASSET_DATA_FORMAT_UNSUPPORTED", "ATS_OUTPUT_PATH_ESCAPE",
		"ATS_RENDERER_NOT_READY", "ATS_SOURCE_LOAD_FAILED", "ATS_MATERIAL_LOAD_FAILED", "ATS_MATERIAL_NO_PREVIEW",
		"ATS_CHARACTER_BIND_POSE_FAILED",
		"ATS_CHARACTER_RESOURCES_UNAVAILABLE",
		"ATS_SOURCE_BOUNDS_INVALID", "ATS_RENDER_FRAME_FAILED",
		"ATS_STREAMING_TIMEOUT_CAPTURED", "ATS_CAPTURE_FAILED", "ATS_DOWNSCALE_FAILED",
		"ATS_TEMP_CLEANUP_FAILED", "ATS_OUTPUT_READBACK_FAILED", "ATS_PARTIAL_FAILURE"
	};
	operation.requestSchemaJson = R"json({"type":"object","additionalProperties":false,"properties":{"cmd":{"const":"asset_thumbnail_studio.generate"},"requestId":{"type":"string","minLength":1,"maxLength":64,"pattern":"^[A-Za-z0-9._-]{1,64}$"},"metadataPaths":{"type":"array","minItems":1,"maxItems":8,"items":{"type":"string","minLength":1}}},"required":["cmd","requestId","metadataPaths"]})json";
	operation.responseSchemaJson = R"json({"type":"object","required":["ok","protocol","operation","requestId","replayed","summary","items","safety","diagnostics"],"successKeys":["summary","items","safety"]})json";
	operation.errorSchemaJson = R"json({"type":"object","required":["ok","diagnostics"],"properties":{"ok":{"const":false},"diagnostics":{"type":"array"}}})json";
	operation.effects.reads = {
		"IEditor::IsDocumentReady", "ILevelEditor::IsLevelLoaded", "CAssetManager::IsScanning",
		"CAssetManager::FindAssetForMetadata", "CAsset::GetType/GetFile/GetThumbnailPath/GetMetadataFile",
		"I3DEngine::LoadStatObj", "ICharacterManager::CreateInstance", "IMaterialManager::LoadMaterial",
		"material_preview_on_mtlsphere", "character_bind_pose", "character_aabb_convergence",
		"renderer_texture_streaming_state", "final_png_bytes_and_dimensions"
	};
	operation.effects.writes = {
		"project_asset.thumbnail_file_at_CAsset::GetThumbnailPath",
		"transient_thumbnail_512_tmp_png", "asset_browser_thumbnail_invalidation"
	};
	operation.effects.mutatesEngine = false;
	operation.effects.writesFiles = true;
	operation.effects.requiresActivePackage = false;
	operation.evidence.maturity = "experimental";
	operation.evidence.docs.push_back("Code/Sandbox/Plugins/AssetThumbnailStudio/doc/CRYAGENT-THUMBNAIL-COMMAND-SPEC.md");
	operation.evidence.source = {
		"AssetThumbnailStudio.dll:CryAgentThumbnailExtension.cpp",
		"AssetThumbnailStudio.dll:ThumbnailGenerationService.cpp",
		"AssetThumbnailStudio.dll:ThumbnailStageRenderer.cpp"
	};
	operation.evidence.limitations = {
		"Available through CryAgentSDKHost, not CryAgentEditorHost port 29931.",
		"Character sources are rendered in their processed bind pose; no animation clip is selected.",
		"Character framing waits for two stable post-render AABB samples, with the shared 10-second capture-gate fallback.",
		"Materials with MTL_FLAG_NOPREVIEW are skipped without writing a thumbnail.",
		"Synchronous main-thread batches can block Sandbox while rendering."
	};
	operation.sideEffects = true;
	operation.transactional = false;
	operation.engineBacked = true;
	registration.handler.reset(new CGenerateThumbnailHandler(m_service));
	return registrar.RegisterOperation(registration, error);
}

} // namespace AssetThumbnailStudio
