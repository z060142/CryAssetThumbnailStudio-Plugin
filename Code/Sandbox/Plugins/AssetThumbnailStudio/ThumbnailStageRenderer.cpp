// Copyright 2026
#include "StdAfx.h"
#include "MitchellDownscaler.h"
#include "StudioAssets.h"
#include "ThumbnailStageRenderer.h"

#include <AssetSystem/Asset.h>
#include <PathUtils.h>
#include <RenderLock.h>

#include <Cry3DEngine/I3DEngine.h>
#include <Cry3DEngine/IMaterial.h>
#include <Cry3DEngine/IStatObj.h>
#include <CryAnimation/IAttachment.h>
#include <CryAnimation/ICryAnimation.h>
#include <CryRenderer/IRenderMesh.h>
#include <CryRenderer/ITexture.h>
#include <CrySystem/IStreamEngine.h>

#include <QDir>
#include <QCryptographicHash>
#include <QFile>
#include <QWidget>

#include <algorithm>
#include <cmath>
#include <vector>

namespace AssetThumbnailStudio
{
namespace
{

// Enabled only while collecting the Step 3 side-by-side quality evidence.
// The final production build keeps the comparison utility but does not emit a sibling file.
constexpr bool kWriteQtReferenceForStep3Evidence = false;

constexpr float kCameraFovDegrees = 30.0f;
constexpr float kFrameMargin = 1.08f;
// Material previews use the sphere inside the tight-fit AABB cube. 0.81
// reduces the measured 19.14% round-1 border to roughly half without
// changing the framing of any non-Material source.
constexpr float kFrameMarginMaterial = 0.81f;
constexpr float kCameraAzimuthDegrees = 60.0f;
constexpr float kCameraVerticalDirection = -0.1608f;
constexpr float kCameraVerticalDirectionMaterial = -0.21f;
constexpr float kMinimumNearPlane = 0.02f;
constexpr float kNearPlaneDistanceScale = 0.001f;
constexpr float kMinimumFarPlane = 4000.0f;
constexpr float kFarPlaneRadiusPadding = 3.0f;
constexpr float kSkyDomeRadius = 500.0f;
constexpr float kLargeStageRadiusThreshold = 128.0f;
constexpr float kEnvironmentAmbientIntensity = 0.22f;
constexpr float kEnvironmentProbeIntensity = 0.9f;

void ForceThumbnailMaterialTextureLoading(IMaterial* pMaterial, int screenTexels)
{
	if (!pMaterial)
	{
		return;
	}

	pMaterial->ForceTexturesLoading(screenTexels);
	// The renderer's screen-texel heuristic can legally settle on a visibly
	// coarse mip for a 4K source viewed through the small off-screen context.
	// Preserve the material-level request as a hint; Step 4.6's direct per-texture
	// mip-0 requests below are the authoritative full-residency mechanism.
	pMaterial->ForceTexturesLoading(0.0f);
}

void AppendUniqueTexture(std::vector<ITexture*>& textures, ITexture* pTexture)
{
	if (pTexture && std::find(textures.begin(), textures.end(), pTexture) == textures.end())
	{
		textures.push_back(pTexture);
	}
}

void CollectMaterialTextures(const std::vector<IMaterial*>& materials, std::vector<ITexture*>& textures)
{
	for (IMaterial* const pMaterial : materials)
	{
		if (pMaterial)
		{
			if (IRenderShaderResources* const pResources = pMaterial->GetShaderItem().m_pShaderResources)
			{
				for (int textureSlot = 0; textureSlot < EFTT_MAX; ++textureSlot)
				{
					if (SEfResTexture* const pTextureResource = pResources->GetTexture(textureSlot))
					{
						AppendUniqueTexture(textures, pTextureResource->m_Sampler.m_pITex);
					}
				}
			}
		}
	}
}

size_t CountPendingFullMipTextures(const std::vector<ITexture*>& textures)
{
	size_t pendingTextureCount = 0;
	for (ITexture* const pTexture : textures)
	{
		if (pTexture->IsStreamable()
			&& (!pTexture->IsTextureLoaded() || pTexture->GetMinLoadedMip() > 0))
		{
			++pendingTextureCount;
		}
	}
	return pendingTextureCount;
}

size_t CountStreamableTextures(const std::vector<ITexture*>& textures)
{
	return static_cast<size_t>(std::count_if(textures.begin(), textures.end(), [](const ITexture* pTexture)
	{
		return pTexture->IsStreamable();
	}));
}

ITexture* FindLargestStreamableTexture(const std::vector<ITexture*>& textures)
{
	ITexture* pLargestTexture = nullptr;
	uint64 largestTexelCount = 0;
	for (ITexture* const pTexture : textures)
	{
		const uint64 texelCount = static_cast<uint64>(max(1, pTexture->GetWidth()))
			* static_cast<uint64>(max(1, pTexture->GetHeight()))
			* static_cast<uint64>(max(1, pTexture->GetDepth()));
		if (pTexture->IsStreamable() && (!pLargestTexture || texelCount > largestTexelCount))
		{
			pLargestTexture = pTexture;
			largestTexelCount = texelCount;
		}
	}
	return pLargestTexture;
}

void AppendUniqueMaterial(std::vector<IMaterial*>& materials, IMaterial* pMaterial)
{
	if (pMaterial && std::find(materials.begin(), materials.end(), pMaterial) == materials.end())
	{
		materials.push_back(pMaterial);
	}
}

void CollectChunkMaterials(IMaterial* pRootMaterial, TRenderChunkArray& chunks, std::vector<IMaterial*>& materials)
{
	if (!pRootMaterial)
	{
		return;
	}

	for (const CRenderChunk& chunk : chunks)
	{
		if (chunk.nNumIndices == 0 || chunk.nNumVerts == 0)
		{
			continue;
		}
		AppendUniqueMaterial(materials, pRootMaterial->GetSafeSubMtl(chunk.m_nMatID));
	}
}

void CollectUsedStatObjMaterials(IStatObj& object, std::vector<IMaterial*>& materials)
{
	IMaterial* const pRootMaterial = object.GetMaterial();
	const size_t materialCountBeforeObject = materials.size();
	if (IRenderMesh* const pRenderMesh = object.GetRenderMesh())
	{
		CollectChunkMaterials(pRootMaterial, pRenderMesh->GetChunks(), materials);
		CollectChunkMaterials(pRootMaterial, pRenderMesh->GetChunksSkinned(), materials);
	}
	if (materials.size() == materialCountBeforeObject)
	{
		AppendUniqueMaterial(materials, pRootMaterial);
	}

	for (int subObjectIndex = 0; subObjectIndex < object.GetSubObjectCount(); ++subObjectIndex)
	{
		if (IStatObj::SSubObject* const pSubObject = object.GetSubObject(subObjectIndex))
		{
			if (pSubObject->pStatObj)
			{
				CollectUsedStatObjMaterials(*pSubObject->pStatObj, materials);
			}
		}
	}
}

void CollectRenderMeshMaterials(IRenderMesh* pRenderMesh, IMaterial* pRootMaterial, std::vector<IMaterial*>& materials)
{
	const size_t materialCountBeforeMesh = materials.size();
	if (pRenderMesh)
	{
		CollectChunkMaterials(pRootMaterial, pRenderMesh->GetChunks(), materials);
		CollectChunkMaterials(pRootMaterial, pRenderMesh->GetChunksSkinned(), materials);
	}
	if (materials.size() == materialCountBeforeMesh)
	{
		AppendUniqueMaterial(materials, pRootMaterial);
	}
}

void CollectMaterialHierarchy(IMaterial* pMaterial, std::vector<IMaterial*>& materials)
{
	if (!pMaterial)
	{
		return;
	}
	AppendUniqueMaterial(materials, pMaterial);
	for (int subMaterialIndex = 0; subMaterialIndex < pMaterial->GetSubMtlCount(); ++subMaterialIndex)
	{
		CollectMaterialHierarchy(pMaterial->GetSubMtl(subMaterialIndex), materials);
	}
}

void CollectUsedCharacterMaterials(ICharacterInstance& character, std::vector<IMaterial*>& materials,
	std::vector<ICharacterInstance*>& visitedCharacters)
{
	if (std::find(visitedCharacters.begin(), visitedCharacters.end(), &character) != visitedCharacters.end())
	{
		return;
	}
	visitedCharacters.push_back(&character);

	CollectRenderMeshMaterials(character.GetIDefaultSkeleton().GetIRenderMesh(), character.GetIMaterial(), materials);
	CollectMaterialHierarchy(character.GetIMaterial(), materials);
	if (IAttachmentManager* const pAttachmentManager = character.GetIAttachmentManager())
	{
		for (int attachmentIndex = 0; attachmentIndex < pAttachmentManager->GetAttachmentCount(); ++attachmentIndex)
		{
			IAttachment* const pAttachment = pAttachmentManager->GetInterfaceByIndex(attachmentIndex);
			if (!pAttachment)
			{
				continue;
			}
			if (IAttachmentSkin* const pAttachmentSkin = pAttachment->GetIAttachmentSkin())
			{
				ISkin* const pSkin = pAttachmentSkin->GetISkin();
				IMaterial* const pSkinMaterial = pSkin ? pSkin->GetIMaterial(0) : nullptr;
				CollectRenderMeshMaterials(pSkin ? pSkin->GetIRenderMesh(0) : nullptr, pSkinMaterial, materials);
				CollectMaterialHierarchy(pSkinMaterial, materials);
			}

			IAttachmentObject* const pAttachmentObject = pAttachment->GetIAttachmentObject();
			if (!pAttachmentObject)
			{
				continue;
			}
			CollectMaterialHierarchy(pAttachmentObject->GetBaseMaterial(0), materials);

			if (IStatObj* const pStatObj = pAttachmentObject->GetIStatObj())
			{
				CollectUsedStatObjMaterials(*pStatObj, materials);
			}
			else if (ICharacterInstance* const pAttachedCharacter = pAttachmentObject->GetICharacterInstance())
			{
				CollectUsedCharacterMaterials(*pAttachedCharacter, materials, visitedCharacters);
			}
			else if (IAttachmentSkin* const pAttachmentSkin = pAttachmentObject->GetIAttachmentSkin())
			{
				ISkin* const pSkin = pAttachmentSkin->GetISkin();
				CollectRenderMeshMaterials(pSkin ? pSkin->GetIRenderMesh(0) : nullptr,
					pAttachmentObject->GetBaseMaterial(0), materials);
			}
		}
	}
}

void CollectUsedCharacterMaterials(ICharacterInstance& character, std::vector<IMaterial*>& materials)
{
	std::vector<ICharacterInstance*> visitedCharacters;
	CollectUsedCharacterMaterials(character, materials, visitedCharacters);
}

bool CharacterRenderResourcesReady(ICharacterInstance& character, std::vector<ICharacterInstance*>& visitedCharacters)
{
	if (std::find(visitedCharacters.begin(), visitedCharacters.end(), &character) != visitedCharacters.end())
	{
		return true;
	}
	visitedCharacters.push_back(&character);

	bool hasRenderableMesh = character.GetIDefaultSkeleton().GetIRenderMesh() != nullptr;
	bool hasPendingAttachmentMesh = false;
	if (IAttachmentManager* const pAttachmentManager = character.GetIAttachmentManager())
	{
		for (int attachmentIndex = 0; attachmentIndex < pAttachmentManager->GetAttachmentCount(); ++attachmentIndex)
		{
			IAttachment* const pAttachment = pAttachmentManager->GetInterfaceByIndex(attachmentIndex);
			if (!pAttachment)
			{
				continue;
			}
			if (IAttachmentSkin* const pAttachmentSkin = pAttachment->GetIAttachmentSkin())
			{
				ISkin* const pSkin = pAttachmentSkin->GetISkin();
				if (pSkin && pSkin->GetIRenderMesh(0))
				{
					hasRenderableMesh = true;
				}
				else
				{
					hasPendingAttachmentMesh = true;
				}
			}

			IAttachmentObject* const pAttachmentObject = pAttachment->GetIAttachmentObject();
			if (!pAttachmentObject)
			{
				continue;
			}
			if (pAttachmentObject->GetIStatObj())
			{
				hasRenderableMesh = true;
			}
			else if (ICharacterInstance* const pAttachedCharacter = pAttachmentObject->GetICharacterInstance())
			{
				const bool attachedReady = CharacterRenderResourcesReady(*pAttachedCharacter, visitedCharacters);
				hasRenderableMesh = hasRenderableMesh || attachedReady;
				hasPendingAttachmentMesh = hasPendingAttachmentMesh || !attachedReady;
			}
		}
	}
	return hasRenderableMesh && !hasPendingAttachmentMesh;
}

bool CharacterRenderResourcesReady(ICharacterInstance& character)
{
	std::vector<ICharacterInstance*> visitedCharacters;
	return CharacterRenderResourcesReady(character, visitedCharacters);
}

AABB SampleCharacterCaptureAabb(ICharacterInstance& character, bool& renderMeshBoundsUsed)
{
	AABB captureAabb = character.GetAABB();
	IRenderMesh* const pRenderMesh = character.GetIDefaultSkeleton().GetIRenderMesh();
	if (!pRenderMesh)
	{
		return captureAabb;
	}

	Vec3 renderMeshMin;
	Vec3 renderMeshMax;
	pRenderMesh->GetBBox(renderMeshMin, renderMeshMax);
	const AABB renderMeshAabb(renderMeshMin, renderMeshMax);
	if (!renderMeshMin.IsValid() || !renderMeshMax.IsValid() || renderMeshAabb.IsReset() || renderMeshAabb.IsEmpty())
	{
		return captureAabb;
	}

	renderMeshBoundsUsed = true;
	if (captureAabb.IsReset() || captureAabb.IsEmpty())
	{
		return renderMeshAabb;
	}
	captureAabb.Add(renderMeshAabb);
	return captureAabb;
}

bool IsStatObjectExtension(const char* szExtension)
{
	return stricmp(szExtension, "cgf") == 0;
}

bool IsCharacterExtension(const char* szExtension)
{
	return stricmp(szExtension, "cga") == 0
		|| stricmp(szExtension, "skin") == 0
		|| stricmp(szExtension, "chr") == 0
		|| stricmp(szExtension, "skel") == 0
		|| stricmp(szExtension, "cdf") == 0;
}

bool IsMaterialExtension(const char* szExtension)
{
	return szExtension && stricmp(szExtension, "mtl") == 0;
}

bool MaterialDisablesPreview(const char* szMaterialPath)
{
	if (!szMaterialPath || !gEnv || !gEnv->pSystem)
	{
		return false;
	}

	// MTL_FLAG_NOPREVIEW is editor-only and intentionally absent from
	// MTL_FLAGS_SAVE_MASK, so the runtime CMatMan drops it while loading the
	// render material. Read the serialized root flag to match CMaterial and
	// QPreviewWidget behavior without depending on EditorQt implementation types.
	const XmlNodeRef materialNode = gEnv->pSystem->LoadXmlFromFile(szMaterialPath);
	int serializedFlags = 0;
	return materialNode && materialNode->getAttr("MtlFlags", serializedFlags)
		&& (serializedFlags & MTL_FLAG_NOPREVIEW) != 0;
}

} // namespace

CThumbnailStageRenderer::CThumbnailStageRenderer()
	: m_pRenderer(GetIEditor() ? GetIEditor()->GetRenderer() : nullptr)
	, m_groundMatrix(IDENTITY)
	, m_skyDomeMatrix(IDENTITY)
{
	m_camera.SetFrustum(kOutputSize, kOutputSize, DEG2RAD(kCameraFovDegrees), kMinimumNearPlane, kMinimumFarPlane);

	m_sun.m_Flags = DLF_SUN | DLF_DIRECTIONAL | DLF_CASTSHADOW_MAPS;
	m_sun.SetLightColor(ColorF(1.0f, 1.0f, 1.0f, 1.0f));
	m_sun.SetRadius(10000.0f);
	// The secondary viewport treats this position as the directional-light vector.
	// Keep it on the camera-facing side so the minimal Step 2 stage remains legible
	// without introducing a fill light or an environment probe.
	m_sun.SetPosition(Vec3(-100.0f, -100.0f, 150.0f));
	if (gEnv && gEnv->pConsole)
	{
		if (ICVar* const pShadows = gEnv->pConsole->GetCVar("e_Shadows"))
		{
			m_shadowsCVarValue = pShadows->GetIVal();
		}
	}
	CryLog("[AssetThumbnailStudio] Step 5 shadow request: pipeline=CharacterTool castShadowMaps=true e_Shadows=%d ambient=%.2f probeIntensity=%.2f.",
		m_shadowsCVarValue, kEnvironmentAmbientIntensity, kEnvironmentProbeIntensity);

	CreateContext();
}

CThumbnailStageRenderer::~CThumbnailStageRenderer()
{
	DestroyContext();
}

bool CThumbnailStageRenderer::CreateContext()
{
	if (!m_pRenderer || !GetIEditor() || !GetIEditor()->Get3DEngine())
	{
		CryWarning(VALIDATOR_MODULE_EDITOR, VALIDATOR_WARNING,
			"[AssetThumbnailStudio] Cannot create the thumbnail renderer: editor rendering services are unavailable.");
		return false;
	}

	m_pRenderWindow = std::make_unique<QWidget>();
	m_pRenderWindow->setWindowFlags(Qt::MSWindowsOwnDC);
	m_pRenderWindow->setAttribute(Qt::WA_PaintOnScreen);
	m_pRenderWindow->setAttribute(Qt::WA_NativeWindow);
	m_pRenderWindow->resize(kOutputSize, kOutputSize);

	IRenderer::SDisplayContextDescription contextDescription;
	contextDescription.handle = reinterpret_cast<HWND>(m_pRenderWindow->winId());
	contextDescription.type = IRenderer::eViewportType_Secondary;
	contextDescription.clearColor = ColorF(0.2f, 0.2f, 0.2f, 1.0f);
	contextDescription.renderFlags = FRT_CLEAR | FRT_OVERLAY_DEPTH;
	contextDescription.screenResolution = { kOutputSize, kOutputSize };

	m_displayContextKey = m_pRenderer->CreateSwapChainBackedContext(contextDescription);
	m_contextCreated = true;

	m_pipelineDescription.type = EGraphicsPipelineType::CharacterTool;
	m_pipelineDescription.shaderFlags = SHDF_SECONDARY_VIEWPORT | SHDF_ALLOWHDR | SHDF_ALLOWPOSTPROCESS
	                                  | SHDF_ALLOW_AO | SHDF_ZPASS | SHDF_ALLOW_SKY;
	m_graphicsPipelineKey = m_pRenderer->CreateGraphicsPipeline(m_pipelineDescription);
	if (m_graphicsPipelineKey == SGraphicsPipelineKey::InvalidGraphicsPipelineKey)
	{
		CryWarning(VALIDATOR_MODULE_EDITOR, VALIDATOR_WARNING,
			"[AssetThumbnailStudio] Failed to create the CharacterTool graphics pipeline.");
		DestroyContext();
		return false;
	}

	m_pGround = GetIEditor()->Get3DEngine()->LoadStatObj(kGroundPlateCgf, nullptr, nullptr, false);
	if (!m_pGround)
	{
		CryWarning(VALIDATOR_MODULE_EDITOR, VALIDATOR_WARNING,
			"[AssetThumbnailStudio] Failed to load the Step 3 ground plate '%s'.", kGroundPlateCgf);
		DestroyContext();
		return false;
	}
	if (!RunMitchellCheckerboardSelfTest())
	{
		CryWarning(VALIDATOR_MODULE_EDITOR, VALIDATOR_ERROR,
			"[AssetThumbnailStudio] Mitchell checkerboard self-test failed; refusing to emit invalid thumbnails.");
		DestroyContext();
		return false;
	}
	CryLog("[AssetThumbnailStudio] Mitchell checkerboard self-test passed (8x8 to 4x4, grayscale preserved).");

	const AABB groundAabb = m_pGround->GetAABB();
	m_groundTopZ = groundAabb.max.z;
	m_groundMatrix = Matrix34::CreateTranslationMat(Vec3(0.0f, 0.0f, -m_groundTopZ));
	if (IMaterial* const pGroundMaterial = m_pGround->GetMaterial())
	{
		pGroundMaterial->PrecacheMaterial(0.0f, nullptr, true, true);
		pGroundMaterial->ForceTexturesLoading(kOutputSize);
	}

	CryLog("[AssetThumbnailStudio] Ground '%s' AABB min=(%.3f, %.3f, %.3f) max=(%.3f, %.3f, %.3f), zOffset=%.3f.",
		kGroundPlateCgf,
		groundAabb.min.x, groundAabb.min.y, groundAabb.min.z,
		groundAabb.max.x, groundAabb.max.y, groundAabb.max.z,
		m_groundMatrix.GetTranslation().z);

	m_pSkyDome = GetIEditor()->Get3DEngine()->LoadStatObj(kSkyDomeCgf, nullptr, nullptr, false);
	if (!m_pSkyDome)
	{
		CryWarning(VALIDATOR_MODULE_EDITOR, VALIDATOR_WARNING,
			"[AssetThumbnailStudio] Failed to load the Step 4 sky dome '%s'.", kSkyDomeCgf);
		DestroyContext();
		return false;
	}

	IMaterialManager* const pMaterialManager = GetIEditor()->Get3DEngine()->GetMaterialManager();
	IMaterial* const pSourceSkyMaterial = pMaterialManager ? pMaterialManager->LoadMaterial(kSkyboxMtl, false) : nullptr;
	if (!pSourceSkyMaterial || !pMaterialManager)
	{
		CryWarning(VALIDATOR_MODULE_EDITOR, VALIDATOR_WARNING,
			"[AssetThumbnailStudio] Failed to load the Step 4 sky material '%s'.", kSkyboxMtl);
		DestroyContext();
		return false;
	}
	m_pSkyMaterial = pMaterialManager->CloneMaterial(pSourceSkyMaterial);
	if (!m_pSkyMaterial)
	{
		CryWarning(VALIDATOR_MODULE_EDITOR, VALIDATOR_WARNING,
			"[AssetThumbnailStudio] Failed to clone the Step 4 sky material '%s'.", kSkyboxMtl);
		DestroyContext();
		return false;
	}
	// The editor preview sphere has outward-facing normals. Keep the shared source
	// material untouched and mark only this private clone two-sided. The negative
	// uniform scale below also reverses winding, making the sphere interior-facing
	// without depending on renderer-private shader-resource mutation APIs.
	m_pSkyMaterial->SetFlags(m_pSkyMaterial->GetFlags() | MTL_FLAG_2SIDED);
	m_pSkyMaterial->PrecacheMaterial(0.0f, nullptr, true, true);
	ForceThumbnailMaterialTextureLoading(m_pSkyMaterial, kOutputSize);

	// Material thumbnails use the same inward-facing dome geometry, but their
	// background is a private clone whose diffuse slot is the engine grey texture.
	// This keeps the shared project sky material and all non-Material thumbnails
	// unchanged while avoiding a flat renderer clear color behind the preview.
	m_pMaterialBackgroundTexture = m_pRenderer->EF_LoadTexture(kMaterialBackgroundTextureDds, 0);
	m_pMaterialBackgroundMaterial = pMaterialManager->CloneMaterial(m_pSkyMaterial);
	if (!m_pMaterialBackgroundTexture || !m_pMaterialBackgroundMaterial)
	{
		CryWarning(VALIDATOR_MODULE_EDITOR, VALIDATOR_WARNING,
			"[AssetThumbnailStudio] Failed to create the Material background from '%s'.",
			kMaterialBackgroundTextureDds);
		DestroyContext();
		return false;
	}
	m_pMaterialBackgroundMaterial->SetTexture(m_pMaterialBackgroundTexture->GetTextureID(), EFTT_DIFFUSE);
	m_pMaterialBackgroundMaterial->PrecacheMaterial(0.0f, nullptr, true, true);
	ForceThumbnailMaterialTextureLoading(m_pMaterialBackgroundMaterial, kOutputSize);
	// Reverse one horizontal axis to keep the outward preview sphere usable as
	// an inward-facing dome without also turning the sky texture upside down.
	// A negative uniform scale reversed X/Y/Z; preserving positive Z keeps the
	// source material's vertical orientation intact.
	m_skyDomeMatrix = Matrix34::CreateScale(Vec3(-kSkyDomeRadius, kSkyDomeRadius, kSkyDomeRadius));

	m_probe.SetPosition(Vec3(0.0f, 0.0f, 0.0f));
	m_probe.SetRadius(10000.0f, 0);
	m_probe.SetLightColor(ColorF(1.0f, 1.0f, 1.0f, 1.0f));
	m_probe.m_ProbeExtents = Vec3(10000.0f);
	m_probe.m_fBoxWidth = m_probe.m_fBoxLength = m_probe.m_fBoxHeight = 5000.0f;
	m_probe.m_Flags = DLF_DEFERRED_CUBEMAPS;
	m_probe.SetMatrix(Matrix34::CreateIdentity());
	m_probe.SetSpecularCubemap(m_pRenderer->EF_LoadTexture(kProbeSpecDds, 0));
	m_probe.SetDiffuseCubemap(m_pRenderer->EF_LoadTexture(kProbeDiffDds, 0));
	const bool specularLoaded = m_probe.GetSpecularCubemap() && m_probe.GetSpecularCubemap()->IsTextureLoaded();
	const bool diffuseLoaded = m_probe.GetDiffuseCubemap() && m_probe.GetDiffuseCubemap()->IsTextureLoaded();
	if (!specularLoaded || !diffuseLoaded)
	{
		CryWarning(VALIDATOR_MODULE_EDITOR, VALIDATOR_WARNING,
			"[AssetThumbnailStudio] Environment probe disabled: specular '%s' loaded=%s, diffuse '%s' loaded=%s. Thumbnail generation will continue.",
			kProbeSpecDds, specularLoaded ? "true" : "false", kProbeDiffDds, diffuseLoaded ? "true" : "false");
		m_probe.m_Flags |= DLF_DISABLED;
		m_probe.ReleaseCubemaps();
		m_probeEnabled = false;
	}
	else
	{
		m_probeEnabled = true;
		CryLog("[AssetThumbnailStudio] Environment probe enabled with specular '%s' and diffuse '%s'.",
			kProbeSpecDds, kProbeDiffDds);
	}

	CryLog("[AssetThumbnailStudio] Sky dome '%s' uses material '%s' at radius %.1f (private two-sided clone).",
		kSkyDomeCgf, kSkyboxMtl, kSkyDomeRadius);
	CryLog("[AssetThumbnailStudio] Material background uses private sky clone with diffuse texture '%s'.",
		kMaterialBackgroundTextureDds);

	CryLog("[AssetThumbnailStudio] Created %dx%d off-screen thumbnail context.", kOutputSize, kOutputSize);
	return true;
}

void CThumbnailStageRenderer::DestroyContext()
{
	m_probeEnabled = false;
	m_probe.ReleaseCubemaps();
	m_pMaterialBackgroundTexture = nullptr;
	m_pMaterialBackgroundMaterial = nullptr;
	m_pSkyMaterial = nullptr;
	m_pSkyDome = nullptr;
	m_pGround = nullptr;

	if (m_pRenderer && m_contextCreated)
	{
		m_pRenderer->DeleteContext(m_displayContextKey);
		if (m_graphicsPipelineKey != SGraphicsPipelineKey::InvalidGraphicsPipelineKey)
		{
			m_pRenderer->DeleteGraphicsPipeline(m_graphicsPipelineKey);
		}
	}

	m_graphicsPipelineKey = SGraphicsPipelineKey::InvalidGraphicsPipelineKey;
	m_contextCreated = false;
	m_groundMatrix.SetIdentity();
	m_groundTopZ = 0.0f;
	m_skyDomeMatrix.SetIdentity();
	m_skyDomeRadius = 0.0f;
	m_skyDomeCameraDistance = 0.0f;
	m_stageScale = 1.0f;
	m_pRenderWindow.reset();
}

void CThumbnailStageRenderer::ConfigureStage(const AABB& aabb, Matrix34& modelMatrix, bool useCharacterForwardAxis,
	float frameMargin, bool useSphericalPreviewRadius)
{
	const Vec3 center = aabb.GetCenter();
	const float sourceYaw = useCharacterForwardAxis ? gf_PI : 0.0f;
	modelMatrix = Matrix34::CreateRotationZ(sourceYaw);
	const Vec3 rotatedCenter = modelMatrix.TransformVector(center);
	modelMatrix.SetTranslation(Vec3(-rotatedCenter.x, -rotatedCenter.y, -aabb.min.z));

	const Vec3 target(0.0f, 0.0f, (aabb.max.z - aabb.min.z) * 0.5f);
	constexpr float fov = DEG2RAD(kCameraFovDegrees);
	const Vec3 halfExtents = aabb.GetSize() * 0.5f;
	const float previewSphereRadius = max(halfExtents.x, max(halfExtents.y, halfExtents.z));
	const float boundingRadius = max(0.05f, useSphericalPreviewRadius ? previewSphereRadius : aabb.GetRadius());
	const float cameraAzimuth = DEG2RAD(kCameraAzimuthDegrees);
	const float cameraVerticalDirection = useSphericalPreviewRadius ? kCameraVerticalDirectionMaterial : kCameraVerticalDirection;
	const Vec3 direction = Vec3(std::cos(cameraAzimuth), std::sin(cameraAzimuth), cameraVerticalDirection).GetNormalized();
	const Matrix33 cameraOrientation = Matrix33::CreateRotationVDir(direction, 0.0f);
	const Vec3 right = cameraOrientation.GetColumn0();
	const Vec3 up = cameraOrientation.GetColumn2();
	const float tanHalfFov = std::tan(fov * 0.5f);
	float requiredDistance = 0.0f;
	for (int x = 0; x < 2; ++x)
	{
		for (int y = 0; y < 2; ++y)
		{
			for (int z = 0; z < 2; ++z)
			{
				const Vec3 localCorner(
					x == 0 ? aabb.min.x : aabb.max.x,
					y == 0 ? aabb.min.y : aabb.max.y,
					z == 0 ? aabb.min.z : aabb.max.z);
				const Vec3 relativeCorner = modelMatrix.TransformPoint(localCorner) - target;
				const float depth = relativeCorner.Dot(direction);
				const float horizontalHalfExtent = std::fabs(relativeCorner.Dot(right));
				const float verticalHalfExtent = std::fabs(relativeCorner.Dot(up));
				requiredDistance = max(requiredDistance,
					max(horizontalHalfExtent, verticalHalfExtent) / tanHalfFov + depth);
			}
		}
	}
	const float cameraDistance = max(0.1f, requiredDistance * frameMargin);
	const float nearPlane = max(kMinimumNearPlane, cameraDistance * kNearPlaneDistanceScale);

	Matrix34 cameraMatrix = cameraOrientation;
	cameraMatrix.SetTranslation(target - direction * cameraDistance);
	m_skyDomeCameraDistance = cameraMatrix.GetTranslation().GetLength();
	// Past the 128-unit reference radius, scale the entire studio environment
	// with the model. The camera-containment term is a safety floor for shapes
	// whose tight-fit viewing distance grows faster than radius alone.
	m_stageScale = 1.0f;
	if (boundingRadius > kLargeStageRadiusThreshold)
	{
		const float modelScale = boundingRadius / kLargeStageRadiusThreshold;
		const float cameraContainmentScale = (m_skyDomeCameraDistance + boundingRadius) / kSkyDomeRadius;
		m_stageScale = max(modelScale, cameraContainmentScale);
	}
	m_groundMatrix = Matrix34::CreateScale(Vec3(m_stageScale));
	m_groundMatrix.SetTranslation(Vec3(0.0f, 0.0f, -m_groundTopZ * m_stageScale));
	m_skyDomeRadius = kSkyDomeRadius * m_stageScale;
	m_skyDomeMatrix = Matrix34::CreateScale(Vec3(-m_skyDomeRadius, m_skyDomeRadius, m_skyDomeRadius));
	// Looking back through the origin reaches the far side of the sphere after
	// roughly cameraDistance + radius. Keep that surface inside the far plane.
	const float farPlane = max(
		max(kMinimumFarPlane, cameraDistance + boundingRadius * kFarPlaneRadiusPadding),
		m_skyDomeCameraDistance + m_skyDomeRadius + boundingRadius);
	m_camera.SetMatrix(cameraMatrix);
	m_camera.SetFrustum(kOutputSize, kOutputSize, fov, nearPlane, farPlane);
	CryLog("[AssetThumbnailStudio] Camera tight-AABB fit required=%.3f radius=%.3f distance=%.3f fov=%.1f margin=%.3f azimuth=%.1f vertical=%.2f near=%.3f far=%.3f stageScale=%.3f skyRadius=%.3f skyCameraDistance=%.3f expanded=%s.",
		requiredDistance, boundingRadius, cameraDistance, kCameraFovDegrees, frameMargin,
		kCameraAzimuthDegrees, cameraVerticalDirection, nearPlane, farPlane,
		m_stageScale, m_skyDomeRadius, m_skyDomeCameraDistance, m_stageScale > 1.0f ? "true" : "false");
	CryLog("[AssetThumbnailStudio] Source forward-axis normalization yaw=%.1f degrees.", RAD2DEG(sourceYaw));
}

bool CThumbnailStageRenderer::ProcessCharacterBindPose(ICharacterInstance& character)
{
	if (!gEnv || !gEnv->pCharacterManager || !character.GetISkeletonPose())
	{
		return false;
	}

	gEnv->pCharacterManager->Update(false);
	gEnv->pCharacterManager->UpdateStreaming(-1, -1);
	character.GetISkeletonPose()->SetForceSkeletonUpdate(0);
	const float cameraDistance = max(0.001f, m_camera.GetPosition().GetLength());
	const float zoomFactor = 0.001f + 0.999f * (RAD2DEG(m_camera.GetFov()) / 60.0f);
	SAnimationProcessParams params;
	params.locationAnimation = QuatTS(IDENTITY);
	params.bOnRender = 0;
	params.zoomAdjustedDistanceFromCamera = cameraDistance * zoomFactor;
	character.StartAnimationProcessing(params);
	// The stock preview finishes the instance here and relies on the editor's
	// end-of-frame CharacterManager sync to clear the shared processing queue.
	// This renderer performs multiple polls inside one editor tick, so complete
	// and clear the public manager queue synchronously before another sample.
	gEnv->pCharacterManager->SyncAllAnimations();
	return true;
}

bool CThumbnailStageRenderer::RenderFrame(IStatObj* pStatObj, ICharacterInstance* pCharacter, IMaterial* pOverrideMaterial,
	IMaterial* pSkyDomeMaterial, bool renderGround, Matrix34& modelMatrix)
{
	if ((!pStatObj && !pCharacter) || (pStatObj && pCharacter))
	{
		return false;
	}
	CScopedRenderLock renderLock;
	if (!renderLock)
	{
		CryWarning(VALIDATOR_MODULE_EDITOR, VALIDATOR_WARNING,
			"[AssetThumbnailStudio] Skipped a thumbnail frame because another editor viewport is rendering.");
		return false;
	}

	const float previousMissionTime = GetIEditor()->GetCurrentMissionTime();
	GetIEditor()->SetCurrentMissionTime(12.0f);

	m_pRenderer->BeginFrame(m_displayContextKey, m_graphicsPipelineKey);

	SRenderingPassInfo passInfo = SRenderingPassInfo::CreateGeneralPassRenderingInfo(
		m_graphicsPipelineKey, m_camera, SRenderingPassInfo::DEFAULT_FLAGS, true, m_displayContextKey);
	m_lastPassRendersShadows = passInfo.RenderShadows();
	m_pRenderer->EF_StartEf(passInfo);
	m_pRenderer->EF_ADDDlight(&m_sun, passInfo);
	m_pRenderer->EF_AddDeferredLight(m_probe, kEnvironmentProbeIntensity, passInfo);

	const auto renderObject = [&passInfo](IStatObj& object, Matrix34& matrix, IMaterial* pMaterial = nullptr)
	{
		SRendParams renderParams;
		renderParams.AmbientColor = ColorF(kEnvironmentAmbientIntensity, kEnvironmentAmbientIntensity, kEnvironmentAmbientIntensity, 1.0f);
		renderParams.dwFObjFlags = FOB_TRANS_MASK | FOB_NO_FOG;
		renderParams.pMatrix = &matrix;
		renderParams.pPrevMatrix = &matrix;
		renderParams.pMaterial = pMaterial;
		object.Render(renderParams, passInfo);
	};

	if (pSkyDomeMaterial)
	{
		renderObject(*m_pSkyDome, m_skyDomeMatrix, pSkyDomeMaterial);
	}
	if (renderGround)
	{
		renderObject(*m_pGround, m_groundMatrix);
	}
	if (pStatObj)
	{
		renderObject(*pStatObj, modelMatrix, pOverrideMaterial);
	}
	else
	{
		SRendParams renderParams;
		renderParams.AmbientColor = ColorF(kEnvironmentAmbientIntensity, kEnvironmentAmbientIntensity, kEnvironmentAmbientIntensity, 1.0f);
		renderParams.dwFObjFlags = FOB_TRANS_MASK | FOB_NO_FOG;
		renderParams.pMatrix = &modelMatrix;
		renderParams.pPrevMatrix = &modelMatrix;
		renderParams.fDistance = (modelMatrix.GetTranslation() - m_camera.GetPosition()).GetLength();
		GetIEditor()->Get3DEngine()->PrecacheCharacter(nullptr, 1.0f, pCharacter, pCharacter->GetIMaterial(),
			modelMatrix, 0.0f, 1.0f, 4, true, passInfo);
		pCharacter->SetViewdir(m_camera.GetViewdir());
		pCharacter->Render(renderParams, passInfo);
	}

	m_pRenderer->EF_EndEf3D(-1, -1, passInfo, m_pipelineDescription.shaderFlags);
	m_pRenderer->RenderDebug(false);
	m_pRenderer->EndFrame();

	GetIEditor()->SetCurrentMissionTime(previousMissionTime);
	return true;
}

bool CThumbnailStageRenderer::RenderAssetThumbnail(CAsset* pAsset)
{
	return RenderAssetThumbnailDetailed(pAsset).ok;
}

SThumbnailRenderResult CThumbnailStageRenderer::RenderAssetThumbnailDetailed(CAsset* pAsset)
{
	SThumbnailRenderResult result;
	result.pipeline.contextReady = IsReady();
	result.pipeline.skyDomeLoaded = m_pSkyDome != nullptr;
	result.pipeline.skyMaterialLoaded = m_pSkyMaterial != nullptr;
	result.pipeline.skyMaterialTwoSided = m_pSkyMaterial && (m_pSkyMaterial->GetFlags() & MTL_FLAG_2SIDED) != 0;
	result.pipeline.materialBackgroundTexturePath = kMaterialBackgroundTextureDds;
	result.pipeline.materialBackgroundTextureLoaded = m_pMaterialBackgroundTexture && m_pMaterialBackgroundTexture->IsTextureLoaded();
	result.pipeline.probeSpecularLoaded = m_probe.GetSpecularCubemap() && m_probe.GetSpecularCubemap()->IsTextureLoaded();
	result.pipeline.probeDiffuseLoaded = m_probe.GetDiffuseCubemap() && m_probe.GetDiffuseCubemap()->IsTextureLoaded();
	result.pipeline.probeEnabled = m_probeEnabled;
	result.pipeline.graphicsPipeline = "CharacterTool";
	result.pipeline.shadowMapRequested = (m_sun.m_Flags & DLF_CASTSHADOW_MAPS) != 0;
	result.pipeline.shadowsCVarValue = m_shadowsCVarValue;
	result.pipeline.nativeShadowStagesRegistered = false;
	result.pipeline.environmentAmbientIntensity = kEnvironmentAmbientIntensity;
	result.pipeline.environmentProbeIntensity = kEnvironmentProbeIntensity;
	result.pipeline.shadowOutcome = "native_unavailable_owner_ruling_no_blob";
	if (!IsReady() || !pAsset || pAsset->GetFilesCount() == 0)
	{
		result.failureCode = IsReady() ? "ATS_REQUEST_INVALID" : "ATS_RENDERER_NOT_READY";
		return result;
	}

	const string& assetFile = pAsset->GetFile(0);
	const char* const szExtension = PathUtil::GetExt(assetFile.c_str());
	result.pipeline.sourceExtension = szExtension ? szExtension : "";
	const bool isStatObject = IsStatObjectExtension(szExtension);
	const bool isCharacter = IsCharacterExtension(szExtension);
	const bool isMaterial = IsMaterialExtension(szExtension);
	result.pipeline.skyDomeRendered = true;
	result.pipeline.materialBackgroundApplied = isMaterial && m_pMaterialBackgroundMaterial != nullptr;
	result.pipeline.groundRendered = !isMaterial;
	if (!isStatObject && !isCharacter && !isMaterial)
	{
		CryWarning(VALIDATOR_MODULE_EDITOR, VALIDATOR_WARNING,
			"[AssetThumbnailStudio] Unsupported thumbnail source format '%s' for '%s'.",
			szExtension ? szExtension : "", assetFile.c_str());
		result.failureCode = "ATS_ASSET_DATA_FORMAT_UNSUPPORTED";
		return result;
	}

	_smart_ptr<IStatObj> pStatObj;
	_smart_ptr<ICharacterInstance> pCharacter;
	_smart_ptr<IMaterial> pPreviewMaterial;
	if (isMaterial)
	{
		result.pipeline.sourceKind = "material";
		result.pipeline.previewGeometryPath = kSkyDomeCgf;
		pStatObj = GetIEditor()->Get3DEngine()->LoadStatObj(kSkyDomeCgf, nullptr, nullptr, false);
		IMaterialManager* const pMaterialManager = GetIEditor()->Get3DEngine()->GetMaterialManager();
		pPreviewMaterial = pMaterialManager ? pMaterialManager->LoadMaterial(assetFile.c_str(), false) : nullptr;
		if (!pPreviewMaterial)
		{
			CryWarning(VALIDATOR_MODULE_EDITOR, VALIDATOR_WARNING,
				"[AssetThumbnailStudio] ATS_MATERIAL_LOAD_FAILED: Failed to load material source '%s'.", assetFile.c_str());
			result.failureCode = "ATS_MATERIAL_LOAD_FAILED";
			return result;
		}
		if (MaterialDisablesPreview(assetFile.c_str())
			|| (pPreviewMaterial->GetFlags() & MTL_FLAG_NOPREVIEW) != 0)
		{
			result.pipeline.sourceLoaded = pStatObj != nullptr;
			CryWarning(VALIDATOR_MODULE_EDITOR, VALIDATOR_WARNING,
				"[AssetThumbnailStudio] ATS_MATERIAL_NO_PREVIEW: Skipped material '%s' because MTL_FLAG_NOPREVIEW is set.", assetFile.c_str());
			result.failureCode = "ATS_MATERIAL_NO_PREVIEW";
			return result;
		}
	}
	else if (isStatObject)
	{
		result.pipeline.sourceKind = "stat_object";
		pStatObj = GetIEditor()->Get3DEngine()->LoadStatObj(assetFile.c_str(), nullptr, nullptr, false);
	}
	else
	{
		result.pipeline.sourceKind = "character";
		result.pipeline.characterLoadFlags = stricmp(szExtension, "cga") == 0 ? 0 : CA_PreviewMode | CA_CharEditModel;
		pCharacter = gEnv && gEnv->pCharacterManager
			? gEnv->pCharacterManager->CreateInstance(assetFile.c_str(), result.pipeline.characterLoadFlags)
			: nullptr;
	}
	if (!pStatObj && !pCharacter)
	{
		CryWarning(VALIDATOR_MODULE_EDITOR, VALIDATOR_WARNING,
			"[AssetThumbnailStudio] Failed to load %s source '%s'.",
			isMaterial ? "material preview sphere" : (isStatObject ? "static mesh" : "character"), assetFile.c_str());
		result.failureCode = "ATS_SOURCE_LOAD_FAILED";
		return result;
	}
	result.pipeline.sourceLoaded = true;

	if (pCharacter)
	{
		result.pipeline.characterBindPoseProcessed = ProcessCharacterBindPose(*pCharacter);
		if (!result.pipeline.characterBindPoseProcessed)
		{
			result.failureCode = "ATS_CHARACTER_BIND_POSE_FAILED";
			return result;
		}
	}
	AABB modelAabb = pStatObj ? pStatObj->GetAABB() : pCharacter->GetAABB();
	if (modelAabb.IsReset() || modelAabb.IsEmpty())
	{
		CryWarning(VALIDATOR_MODULE_EDITOR, VALIDATOR_WARNING,
			"[AssetThumbnailStudio] Loaded source '%s' returned an invalid AABB.", assetFile.c_str());
		result.failureCode = "ATS_SOURCE_BOUNDS_INVALID";
		return result;
	}
	Matrix34 modelMatrix(IDENTITY);
	ConfigureStage(modelAabb, modelMatrix, pCharacter != nullptr,
		isMaterial ? kFrameMarginMaterial : kFrameMargin, isMaterial);
	IMaterial* const pBackgroundMaterial = isMaterial
		? m_pMaterialBackgroundMaterial.get()
		: m_pSkyMaterial.get();
	if (pCharacter)
	{
		result.pipeline.characterAabbConverged = false;
		result.pipeline.characterAabbSamples = 1;
		result.pipeline.characterInitialAabb = modelAabb;
		result.pipeline.characterFinalAabb = modelAabb;
	}
	CryLog("[AssetThumbnailStudio] Staged '%s' AABB min=(%.3f, %.3f, %.3f) max=(%.3f, %.3f, %.3f), offset=(%.3f, %.3f, %.3f).",
		assetFile.c_str(),
		modelAabb.min.x, modelAabb.min.y, modelAabb.min.z,
		modelAabb.max.x, modelAabb.max.y, modelAabb.max.z,
		modelMatrix.GetTranslation().x, modelMatrix.GetTranslation().y, modelMatrix.GetTranslation().z);
	std::vector<IMaterial*> modelMaterials;
	if (pPreviewMaterial)
	{
		CollectMaterialHierarchy(pPreviewMaterial, modelMaterials);
	}
	else if (pStatObj)
	{
		CollectUsedStatObjMaterials(*pStatObj, modelMaterials);
	}
	else
	{
		CollectUsedCharacterMaterials(*pCharacter, modelMaterials);
	}
	std::vector<IMaterial*> captureMaterials = modelMaterials;
	if (!isMaterial && m_pGround)
	{
		CollectUsedStatObjMaterials(*m_pGround, captureMaterials);
	}
	AppendUniqueMaterial(captureMaterials, pBackgroundMaterial);
	std::vector<ITexture*> captureTextures;
	CollectMaterialTextures(captureMaterials, captureTextures);
	std::vector<ITexture*> modelTextures;
	CollectMaterialTextures(modelMaterials, modelTextures);
	ITexture* const pLargestModelStreamableTexture = FindLargestStreamableTexture(modelTextures);
	for (IMaterial* const pMaterial : captureMaterials)
	{
		pMaterial->PrecacheMaterial(0.0f, nullptr, true, true);
		ForceThumbnailMaterialTextureLoading(pMaterial, kOutputSize);
	}
	m_pRenderer->FlushRTCommands(true, true, true);

	// Step 4.6 full-mip residency gate: material-level ForceTexturesLoading can
	// suppress a finer request in the same streaming round, and GetRequiredMip()
	// merely repeats the streamer's chosen target. Reissue a direct mip-0 request
	// for every visible texture on every poll and require actual mip 0 residency.
	// The global planner count and 10-second guard remain as the environmental gate.
	const CTimeValue streamingStart = gEnv->pTimer->GetAsyncTime();
	// A cold D3D11/streaming path can report initial zero polls before material
	// uploads become observable. The used-render-chunk material list above makes
	// local mip readiness authoritative without waiting on unused sub-materials.
	constexpr float kDemandPropagationSeconds = 0.05f;
	int quietFrames = 0;
	int pollFrames = 0;
	int busyFrames = 0;
	size_t lastRequestCount = 0;
	size_t lastPendingFullMipTextureCount = 0;
	size_t directPrecacheRequestCount = 0;
	int texturePrecacheUpdateId = 0;
	bool poolOverflowObserved = false;
	bool poolOverflowTotallyObserved = false;
	bool streamingTimedOut = false;
	const bool characterNeedsRenderResourceCheck = pCharacter && stricmp(szExtension, "cga") != 0;
	bool characterRenderResourcesReady = !characterNeedsRenderResourceCheck || CharacterRenderResourcesReady(*pCharacter);
	constexpr int kRequiredCharacterAabbStableFrames = 2;
	constexpr float kCharacterAabbConvergenceEpsilon = 1e-4f;
	int characterAabbStableFrames = pCharacter ? 0 : kRequiredCharacterAabbStableFrames;
	while (quietFrames < 2 || characterAabbStableFrames < kRequiredCharacterAabbStableFrames)
	{
		++texturePrecacheUpdateId;
		for (ITexture* const pTexture : captureTextures)
		{
			m_pRenderer->EF_PrecacheResource(pTexture, 0.0f, 0.0f,
				FPR_STARTLOADING | FPR_HIGHPRIORITY, texturePrecacheUpdateId);
			++directPrecacheRequestCount;
		}
		if (!RenderFrame(pStatObj, pCharacter, pPreviewMaterial, pBackgroundMaterial, !isMaterial, modelMatrix))
		{
			result.failureCode = "ATS_RENDER_FRAME_FAILED";
			return result;
		}
		++pollFrames;
		m_pRenderer->FlushRTCommands(true, true, true);
		if (gEnv->pSystem && gEnv->pSystem->GetStreamEngine())
		{
			gEnv->pSystem->GetStreamEngine()->Update();
		}
		if (pCharacter)
		{
			gEnv->pCharacterManager->UpdateStreaming(-1, -1);
			characterRenderResourcesReady = !characterNeedsRenderResourceCheck || CharacterRenderResourcesReady(*pCharacter);
			if (!ProcessCharacterBindPose(*pCharacter))
			{
				result.failureCode = "ATS_CHARACTER_BIND_POSE_FAILED";
				return result;
			}
			bool renderMeshBoundsUsed = false;
			const AABB sampledAabb = SampleCharacterCaptureAabb(*pCharacter, renderMeshBoundsUsed);
			result.pipeline.characterRenderMeshBoundsUsed = result.pipeline.characterRenderMeshBoundsUsed || renderMeshBoundsUsed;
			++result.pipeline.characterAabbSamples;
			if (sampledAabb.IsReset() || sampledAabb.IsEmpty())
			{
				characterAabbStableFrames = 0;
			}
			else if (!IsEquivalent(sampledAabb, modelAabb, kCharacterAabbConvergenceEpsilon))
			{
				modelAabb = sampledAabb;
				result.pipeline.characterFinalAabb = modelAabb;
				characterAabbStableFrames = 0;
				ConfigureStage(modelAabb, modelMatrix, true, kFrameMargin, false);
			}
			else
			{
				characterAabbStableFrames = min(characterAabbStableFrames + 1, kRequiredCharacterAabbStableFrames);
			}
			result.pipeline.characterAabbStableFrames = characterAabbStableFrames;
			result.pipeline.characterAabbConverged = characterAabbStableFrames >= kRequiredCharacterAabbStableFrames;
		}

		STextureStreamingStats streamingStats(false);
		m_pRenderer->EF_Query(EFQ_GetTexStreamingInfo, streamingStats);
		lastRequestCount = streamingStats.nNumStreamingRequests;
		lastPendingFullMipTextureCount = CountPendingFullMipTextures(captureTextures);
		poolOverflowObserved = poolOverflowObserved || streamingStats.bPoolOverflow;
		poolOverflowTotallyObserved = poolOverflowTotallyObserved || streamingStats.bPoolOverflowTotally;
		const float elapsedSeconds = (gEnv->pTimer->GetAsyncTime() - streamingStart).GetSeconds();
		const bool hasObservedPendingWork = lastRequestCount != 0 || lastPendingFullMipTextureCount != 0
			|| !characterRenderResourcesReady;
		if (hasObservedPendingWork)
		{
			quietFrames = 0;
			++busyFrames;
		}
		else if (elapsedSeconds >= kDemandPropagationSeconds)
		{
			++quietFrames;
		}
		else
		{
			quietFrames = 0;
		}

		if (elapsedSeconds > 10.0f)
		{
			streamingTimedOut = true;
			CryWarning(VALIDATOR_MODULE_EDITOR, VALIDATOR_WARNING,
				"[AssetThumbnailStudio] Capture gate did not settle in 10s; capturing anyway ('%s', requests=%llu, localPending=%llu, characterAabbStable=%d/%d).",
				assetFile.c_str(), static_cast<unsigned long long>(lastRequestCount),
				static_cast<unsigned long long>(lastPendingFullMipTextureCount),
				characterAabbStableFrames, kRequiredCharacterAabbStableFrames);
			break;
		}
	}

	const float streamingSeconds = (gEnv->pTimer->GetAsyncTime() - streamingStart).GetSeconds();
	result.streaming.pollFrames = pollFrames;
	result.streaming.busyFrames = busyFrames;
	result.streaming.quietFrames = quietFrames;
	result.streaming.elapsedMs = static_cast<int>(streamingSeconds * 1000.0f + 0.5f);
	result.streaming.lastGlobalRequestCount = lastRequestCount;
	result.streaming.lastLocalPendingMipCount = lastPendingFullMipTextureCount;
	result.streaming.usedTextureCount = captureTextures.size();
	result.streaming.streamableTextureCount = CountStreamableTextures(captureTextures);
	result.streaming.directPrecacheRequestCount = directPrecacheRequestCount;
	const char* const szLargestModelTextureName = pLargestModelStreamableTexture ? pLargestModelStreamableTexture->GetName() : nullptr;
	result.streaming.largestModelStreamableTextureName = szLargestModelTextureName ? szLargestModelTextureName : "";
	result.streaming.largestModelStreamableTextureMinLoadedMip = pLargestModelStreamableTexture ? pLargestModelStreamableTexture->GetMinLoadedMip() : 0;
	result.streaming.poolOverflowObserved = poolOverflowObserved;
	result.streaming.poolOverflowTotallyObserved = poolOverflowTotallyObserved;
	result.streaming.timedOut = streamingTimedOut;
	result.pipeline.characterRenderResourcesReady = characterRenderResourcesReady;
	result.pipeline.skyDomeRadius = m_skyDomeRadius;
	result.pipeline.skyDomeExpanded = m_skyDomeRadius > kSkyDomeRadius;
	result.pipeline.skyDomeCameraDistance = m_skyDomeCameraDistance;
	result.pipeline.skyDomeContainsCamera = m_skyDomeRadius > m_skyDomeCameraDistance;
	result.pipeline.stageScale = m_stageScale;
	result.pipeline.largeStageScaled = m_stageScale > 1.0f;
	if (pCharacter)
	{
		CryLog("[AssetThumbnailStudio] Character AABB convergence for '%s': initialMin=(%.6f, %.6f, %.6f) initialMax=(%.6f, %.6f, %.6f) finalMin=(%.6f, %.6f, %.6f) finalMax=(%.6f, %.6f, %.6f) samples=%d stable=%d converged=%s%s.",
			assetFile.c_str(),
			result.pipeline.characterInitialAabb.min.x, result.pipeline.characterInitialAabb.min.y, result.pipeline.characterInitialAabb.min.z,
			result.pipeline.characterInitialAabb.max.x, result.pipeline.characterInitialAabb.max.y, result.pipeline.characterInitialAabb.max.z,
			result.pipeline.characterFinalAabb.min.x, result.pipeline.characterFinalAabb.min.y, result.pipeline.characterFinalAabb.min.z,
			result.pipeline.characterFinalAabb.max.x, result.pipeline.characterFinalAabb.max.y, result.pipeline.characterFinalAabb.max.z,
			result.pipeline.characterAabbSamples, result.pipeline.characterAabbStableFrames,
			result.pipeline.characterAabbConverged ? "true" : "false", streamingTimedOut ? " (timeout)" : "");
	}
	CryLog("[AssetThumbnailStudio] Texture streaming gate for '%s': materials=%llu polls=%d busy=%d quiet=%d elapsed=%.3fs requests=%llu localPending=%llu%s.",
		assetFile.c_str(), static_cast<unsigned long long>(captureMaterials.size()), pollFrames, busyFrames, quietFrames, streamingSeconds,
		static_cast<unsigned long long>(lastRequestCount),
		static_cast<unsigned long long>(lastPendingFullMipTextureCount), streamingTimedOut ? " (timeout)" : "");
	CryLog("[AssetThumbnailStudio] Full-mip residency for '%s': textures=%llu streamable=%llu directPrecacheRequests=%llu largestModelTexture='%s' largestModelMinLoadedMip=%d poolOverflow=%s poolOverflowTotally=%s.",
		assetFile.c_str(), static_cast<unsigned long long>(captureTextures.size()),
		static_cast<unsigned long long>(result.streaming.streamableTextureCount),
		static_cast<unsigned long long>(directPrecacheRequestCount),
		result.streaming.largestModelStreamableTextureName.c_str(),
		result.streaming.largestModelStreamableTextureMinLoadedMip,
		poolOverflowObserved ? "true" : "false", poolOverflowTotallyObserved ? "true" : "false");
	if (!characterRenderResourcesReady)
	{
		CryWarning(VALIDATOR_MODULE_EDITOR, VALIDATOR_WARNING,
			"[AssetThumbnailStudio] Character render resources did not become ready for '%s'.", assetFile.c_str());
		result.failureCode = "ATS_CHARACTER_RESOURCES_UNAVAILABLE";
		return result;
	}

	// IsParticularMipStreamed() becomes true when the requested mip is available
	// to the renderer, one or more frames before the secondary swap chain is
	// guaranteed to present it. Advance that presentation tail explicitly; this
	// prevents a cold batch from returning a lower-mip first thumbnail followed
	// by a different stable thumbnail for the same asset.
	constexpr int kPostStreamingSettleFrames = 4;
	for (int settleFrame = 0; settleFrame < kPostStreamingSettleFrames; ++settleFrame)
	{
		if (!RenderFrame(pStatObj, pCharacter, pPreviewMaterial, pBackgroundMaterial, !isMaterial, modelMatrix))
		{
			result.failureCode = "ATS_RENDER_FRAME_FAILED";
			return result;
		}
		m_pRenderer->FlushRTCommands(true, true, true);
		if (gEnv->pSystem && gEnv->pSystem->GetStreamEngine())
		{
			gEnv->pSystem->GetStreamEngine()->Update();
		}
	}

	const string absoluteOutputPath = PathUtil::Make(
		PathUtil::GetGameProjectAssetsPath(), pAsset->GetThumbnailPath());
	if (!QDir().mkpath(QtUtil::ToQString(PathUtil::GetPathWithoutFilename(absoluteOutputPath))))
	{
		result.failureCode = "ATS_OUTPUT_READBACK_FAILED";
		return result;
	}
	const string temporaryOutputPath = absoluteOutputPath + ".512.tmp.png";
	const QString temporaryOutputPathQt = QtUtil::ToQString(temporaryOutputPath);
	const QString absoluteOutputPathQt = QtUtil::ToQString(absoluteOutputPath);
	QFile::remove(temporaryOutputPathQt);
	if (QFile::exists(temporaryOutputPathQt))
	{
		result.failureCode = "ATS_TEMP_CLEANUP_FAILED";
		return result;
	}

	// A cold secondary pipeline can continue changing after texture readiness
	// reports quiet. Prove presentation convergence from the capture bytes rather
	// than relying on another fixed sleep/frame count.
	constexpr int kMaximumCaptureAttempts = 8;
	QByteArray previousCaptureHash;
	for (int attempt = 0; attempt < kMaximumCaptureAttempts; ++attempt)
	{
		if (!RenderFrame(pStatObj, pCharacter, pPreviewMaterial, pBackgroundMaterial, !isMaterial, modelMatrix)
			|| !RenderFrame(pStatObj, pCharacter, pPreviewMaterial, pBackgroundMaterial, !isMaterial, modelMatrix))
		{
			result.failureCode = "ATS_RENDER_FRAME_FAILED";
			return result;
		}
		m_pRenderer->FlushRTCommands(true, true, true);
		if (!m_pRenderer->ScreenShot(temporaryOutputPath.c_str(), m_displayContextKey))
		{
			CryWarning(VALIDATOR_MODULE_EDITOR, VALIDATOR_WARNING,
				"[AssetThumbnailStudio] Failed to save 512x512 thumbnail source '%s'.", temporaryOutputPath.c_str());
			QFile::remove(temporaryOutputPathQt);
			result.pipeline.temporaryFileRemoved = !QFile::exists(temporaryOutputPathQt);
			result.failureCode = "ATS_CAPTURE_FAILED";
			return result;
		}
		// ScreenShot may enqueue the readback/write on the render thread. Flush
		// after the call so the hash below observes this attempt, not the previous
		// file contents while an overwrite is still pending.
		m_pRenderer->FlushRTCommands(true, true, true);
		result.pipeline.capture512Succeeded = true;
		result.pipeline.captureAttempts = attempt + 1;
		QFile captureFile(temporaryOutputPathQt);
		if (!captureFile.open(QIODevice::ReadOnly))
		{
			result.failureCode = "ATS_CAPTURE_FAILED";
			return result;
		}
		const QByteArray captureHash = QCryptographicHash::hash(captureFile.readAll(), QCryptographicHash::Sha256);
		captureFile.close();
		if (!previousCaptureHash.isEmpty() && captureHash == previousCaptureHash)
		{
			result.pipeline.capture512Converged = true;
			break;
		}
		previousCaptureHash = captureHash;
	}
	if (!result.pipeline.capture512Converged)
	{
		CryWarning(VALIDATOR_MODULE_EDITOR, VALIDATOR_WARNING,
			"[AssetThumbnailStudio] The 512x512 capture did not converge after %d attempts for '%s'.",
			kMaximumCaptureAttempts, assetFile.c_str());
		QFile::remove(temporaryOutputPathQt);
		result.pipeline.temporaryFileRemoved = !QFile::exists(temporaryOutputPathQt);
		result.failureCode = "ATS_CAPTURE_FAILED";
		return result;
	}

	const bool downscaleSucceeded = DownscaleMitchell(temporaryOutputPathQt, absoluteOutputPathQt, kFinalOutputSize);
	result.pipeline.mitchellDownscaleSucceeded = downscaleSucceeded;
	bool referenceSucceeded = true;
	if (kWriteQtReferenceForStep3Evidence)
	{
		const QString referenceOutputPath = absoluteOutputPathQt + ".qt-smooth.reference.png";
		referenceSucceeded = DownscaleQtSmoothReference(temporaryOutputPathQt, referenceOutputPath, kFinalOutputSize);
		if (referenceSucceeded)
		{
			CryLog("[AssetThumbnailStudio] Wrote temporary Step 3 Qt Smooth comparison '%s'.",
				QtUtil::ToString(referenceOutputPath).c_str());
		}
	}
	QFile::remove(temporaryOutputPathQt);
	result.pipeline.temporaryFileRemoved = !QFile::exists(temporaryOutputPathQt);

	if (!downscaleSucceeded || !referenceSucceeded)
	{
		CryWarning(VALIDATOR_MODULE_EDITOR, VALIDATOR_WARNING,
			"[AssetThumbnailStudio] Failed to produce the Step 3 256x256 output for '%s'.", absoluteOutputPath.c_str());
		result.failureCode = "ATS_DOWNSCALE_FAILED";
		return result;
	}
	if (!result.pipeline.temporaryFileRemoved)
	{
		result.failureCode = "ATS_TEMP_CLEANUP_FAILED";
		return result;
	}

	CryLog("[AssetThumbnailStudio] Saved 256x256 Mitchell thumbnail '%s' for '%s'.",
		absoluteOutputPath.c_str(), pAsset->GetMetadataFile().c_str());
	result.pipeline.renderShadowsPassEnabled = m_lastPassRendersShadows;
	result.pipeline.materialBackgroundTextureLoaded = m_pMaterialBackgroundTexture && m_pMaterialBackgroundTexture->IsTextureLoaded();
	CryLog("[AssetThumbnailStudio] Step 5 shadow outcome for '%s': requested=%s passShadows=%s e_Shadows=%d nativeShadowStages=false blobShadow=false (owner ruling).",
		assetFile.c_str(), result.pipeline.shadowMapRequested ? "true" : "false",
		result.pipeline.renderShadowsPassEnabled ? "true" : "false", result.pipeline.shadowsCVarValue);
	result.ok = true;
	return result;
}

} // namespace AssetThumbnailStudio
