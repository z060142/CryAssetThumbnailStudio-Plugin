// Copyright 2026
#pragma once

#include <CryMath/Cry_Camera.h>
#include <CryCore/smartptr.h>
#include <CryRenderer/IRenderer.h>
#include <CryRenderer/IShader.h>

#include <memory>

class CAsset;
class QWidget;
struct IMaterial;
struct ICharacterInstance;
struct IStatObj;

namespace AssetThumbnailStudio
{

struct SThumbnailStreamingResult
{
	int    pollFrames = 0;
	int    busyFrames = 0;
	int    quietFrames = 0;
	int    elapsedMs = 0;
	size_t lastGlobalRequestCount = 0;
	size_t lastLocalPendingMipCount = 0;
	size_t usedTextureCount = 0;
	size_t streamableTextureCount = 0;
	size_t directPrecacheRequestCount = 0;
	string largestModelStreamableTextureName;
	int    largestModelStreamableTextureMinLoadedMip = 0;
	bool   poolOverflowObserved = false;
	bool   poolOverflowTotallyObserved = false;
	bool   timedOut = false;

	bool QualityGateSatisfied() const { return quietFrames >= 2 && !timedOut; }
};

struct SThumbnailPipelineResult
{
	bool contextReady = false;
	bool sourceLoaded = false;
	string sourceKind;
	string sourceExtension;
	string previewGeometryPath;
	bool characterBindPoseProcessed = false;
	bool characterRenderResourcesReady = false;
	bool characterAabbConverged = true;
	bool characterRenderMeshBoundsUsed = false;
	int  characterAabbSamples = 0;
	int  characterAabbStableFrames = 0;
	AABB characterInitialAabb = AABB(AABB::RESET);
	AABB characterFinalAabb = AABB(AABB::RESET);
	int  characterLoadFlags = 0;
	bool capture512Succeeded = false;
	bool capture512Converged = false;
	int  captureAttempts = 0;
	bool mitchellDownscaleSucceeded = false;
	bool temporaryFileRemoved = false;
	int  outputSize = 256;
	bool skyDomeLoaded = false;
	bool skyDomeRendered = false;
	float skyDomeRadius = 0.0f;
	bool skyDomeExpanded = false;
	bool skyDomeContainsCamera = false;
	float skyDomeCameraDistance = 0.0f;
	float stageScale = 1.0f;
	bool largeStageScaled = false;
	bool skyMaterialLoaded = false;
	bool skyMaterialTwoSided = false;
	string materialBackgroundTexturePath;
	bool materialBackgroundTextureLoaded = false;
	bool materialBackgroundApplied = false;
	bool groundRendered = false;
	bool probeSpecularLoaded = false;
	bool probeDiffuseLoaded = false;
	bool probeEnabled = false;
	string graphicsPipeline;
	bool shadowMapRequested = false;
	int  shadowsCVarValue = -1;
	bool renderShadowsPassEnabled = false;
	bool nativeShadowStagesRegistered = false;
	float environmentAmbientIntensity = 0.0f;
	float environmentProbeIntensity = 0.0f;
	string shadowOutcome;
};

struct SThumbnailRenderResult
{
	bool                      ok = false;
	string                    failureCode;
	SThumbnailStreamingResult streaming;
	SThumbnailPipelineResult  pipeline;
};

class CThumbnailStageRenderer final
{
public:
	CThumbnailStageRenderer();
	~CThumbnailStageRenderer();

	bool IsReady() const { return m_contextCreated; }
	bool RenderAssetThumbnail(CAsset* pAsset);
	SThumbnailRenderResult RenderAssetThumbnailDetailed(CAsset* pAsset);

private:
	bool CreateContext();
	void DestroyContext();
	void ConfigureStage(const AABB& aabb, Matrix34& modelMatrix, bool useCharacterForwardAxis,
		float frameMargin, bool useSphericalPreviewRadius);
	bool ProcessCharacterBindPose(ICharacterInstance& character);
	bool RenderFrame(IStatObj* pStatObj, ICharacterInstance* pCharacter, IMaterial* pOverrideMaterial,
		IMaterial* pSkyDomeMaterial, bool renderGround, Matrix34& modelMatrix);

private:
	static constexpr int kOutputSize = 512;
	static constexpr int kFinalOutputSize = 256;

	IRenderer*                              m_pRenderer = nullptr;
	std::unique_ptr<QWidget>                m_pRenderWindow;
	SDisplayContextKey                      m_displayContextKey;
	SGraphicsPipelineKey                    m_graphicsPipelineKey = SGraphicsPipelineKey::InvalidGraphicsPipelineKey;
	IRenderer::SGraphicsPipelineDescription m_pipelineDescription;
	_smart_ptr<IStatObj>                    m_pGround;
	Matrix34                                m_groundMatrix;
	float                                   m_groundTopZ = 0.0f;
	_smart_ptr<IStatObj>                    m_pSkyDome;
	_smart_ptr<IMaterial>                   m_pSkyMaterial;
	_smart_ptr<IMaterial>                   m_pMaterialBackgroundMaterial;
	_smart_ptr<ITexture>                    m_pMaterialBackgroundTexture;
	Matrix34                                m_skyDomeMatrix;
	float                                   m_skyDomeRadius = 0.0f;
	float                                   m_skyDomeCameraDistance = 0.0f;
	float                                   m_stageScale = 1.0f;
	CCamera                                 m_camera;
	SRenderLight                            m_sun;
	SRenderLight                            m_probe;
	bool                                    m_probeEnabled = false;
	int                                     m_shadowsCVarValue = -1;
	bool                                    m_lastPassRendersShadows = false;
	bool                                    m_contextCreated = false;
};

} // namespace AssetThumbnailStudio
