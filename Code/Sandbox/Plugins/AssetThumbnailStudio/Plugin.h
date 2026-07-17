// Copyright 2026
#pragma once

#include "IPlugin.h"

#include <memory>

namespace AssetThumbnailStudio
{
#if ASSET_THUMBNAIL_STUDIO_WITH_CRYAGENTSDK
class CCryAgentThumbnailExtension;
#endif
class CThumbnailGenerationService;
}

class CAssetThumbnailStudioPlugin final : public IPlugin, public IEditorNotifyListener
{
public:
	CAssetThumbnailStudioPlugin();
	~CAssetThumbnailStudioPlugin() override;

	int32       GetPluginVersion() override     { return 1; }
	const char* GetPluginName() override        { return "Asset Thumbnail Studio"; }
	const char* GetPluginDescription() override { return "Regenerates model asset thumbnails with the Asset Thumbnail Studio renderer."; }
	void OnEditorNotifyEvent(EEditorNotifyEvent event) override;

private:
	std::unique_ptr<AssetThumbnailStudio::CThumbnailGenerationService> m_pGenerationService;
#if ASSET_THUMBNAIL_STUDIO_WITH_CRYAGENTSDK
	std::unique_ptr<AssetThumbnailStudio::CCryAgentThumbnailExtension> m_pCryAgentExtension;
#endif
};
