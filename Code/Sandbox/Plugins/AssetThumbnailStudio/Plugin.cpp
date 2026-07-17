// Copyright 2026
#include "StdAfx.h"
#if ASSET_THUMBNAIL_STUDIO_WITH_CRYAGENTSDK
#include "CryAgentThumbnailExtension.h"
#endif
#include "Plugin.h"
#include "ThumbnailGenerationService.h"

#include <AssetSystem/Asset.h>
#include <AssetSystem/AssetType.h>
#include <AssetSystem/Browser/AssetBrowser.h>
#include <Menu/AbstractMenu.h>

#include <CryCore/Platform/platform_impl.inl>

#include <QAction>
#include <QObject>

#include <cstring>
#include <utility>

namespace AssetThumbnailStudio
{

bool IsSupportedAsset(const CAsset* pAsset)
{
	if (!pAsset || !pAsset->GetType() || pAsset->GetFilesCount() == 0)
	{
		return false;
	}

	const char* const szTypeName = pAsset->GetType()->GetTypeName();
	if (!szTypeName)
	{
		return false;
	}

	return std::strcmp(szTypeName, "Mesh") == 0
	    || std::strcmp(szTypeName, "AnimatedMesh") == 0
	    || std::strcmp(szTypeName, "SkinnedMesh") == 0
	    || std::strcmp(szTypeName, "Skeleton") == 0
	    || std::strcmp(szTypeName, "Character") == 0
	    || std::strcmp(szTypeName, "Material") == 0;
}

void AddAssetBrowserContextMenu(CAbstractMenu& menu, const std::vector<CAsset*>& assets,
	CThumbnailGenerationService& generationService)
{
	std::vector<CAssetPtr> supportedAssets;
	supportedAssets.reserve(assets.size());

	for (CAsset* const pAsset : assets)
	{
		if (IsSupportedAsset(pAsset))
		{
			supportedAssets.emplace_back(pAsset);
		}
	}

	if (supportedAssets.empty())
	{
		return;
	}

	const int section = menu.GetNextEmptySection();
	menu.SetSectionName(section, "Thumbnail Studio");

	QAction* const pAction = menu.CreateAction(
		QObject::tr("Regenerate Thumbnail (Studio) [%1]").arg(supportedAssets.size()), section);
	QObject::connect(pAction, &QAction::triggered,
		[&generationService, supportedAssets = std::move(supportedAssets)]()
	{
		generationService.GenerateForUi(supportedAssets);
	});
}

} // namespace AssetThumbnailStudio

CAssetThumbnailStudioPlugin::CAssetThumbnailStudioPlugin()
	: m_pGenerationService(new AssetThumbnailStudio::CThumbnailGenerationService())
#if ASSET_THUMBNAIL_STUDIO_WITH_CRYAGENTSDK
	, m_pCryAgentExtension(new AssetThumbnailStudio::CCryAgentThumbnailExtension(*m_pGenerationService))
#endif
{
#if ASSET_THUMBNAIL_STUDIO_WITH_CRYAGENTSDK
	m_pCryAgentExtension->Attach();
#endif
	GetIEditor()->RegisterNotifyListener(this);
	CAssetBrowser::s_signalContextMenuRequested.Connect(
		[this](CAbstractMenu& menu, const std::vector<CAsset*>& assets, const std::vector<string>&,
		   const std::shared_ptr<IUIContext>&)
		{
			AssetThumbnailStudio::AddAssetBrowserContextMenu(menu, assets, *m_pGenerationService);
		},
		reinterpret_cast<uintptr_t>(this));
}

CAssetThumbnailStudioPlugin::~CAssetThumbnailStudioPlugin()
{
#if ASSET_THUMBNAIL_STUDIO_WITH_CRYAGENTSDK
	if (m_pCryAgentExtension)
	{
		m_pCryAgentExtension->Detach();
	}
#endif
	GetIEditor()->UnregisterNotifyListener(this);
	CAssetBrowser::s_signalContextMenuRequested.DisconnectById(reinterpret_cast<uintptr_t>(this));
#if ASSET_THUMBNAIL_STUDIO_WITH_CRYAGENTSDK
	m_pCryAgentExtension.reset();
#endif
	m_pGenerationService.reset();
}

void CAssetThumbnailStudioPlugin::OnEditorNotifyEvent(EEditorNotifyEvent event)
{
	if (event == eNotify_OnQuit && m_pGenerationService)
	{
		m_pGenerationService->ShutdownUiQueue();
	}

	// Sandbox editor plugins are constructed before native project plugins on
	// some startup paths. Retry silently from idle until CryAgentSDKHost exists.
#if ASSET_THUMBNAIL_STUDIO_WITH_CRYAGENTSDK
	if (event == eNotify_OnIdleUpdate && m_pCryAgentExtension && !m_pCryAgentExtension->IsAttached())
	{
		m_pCryAgentExtension->Attach();
	}
#endif
}

REGISTER_PLUGIN(CAssetThumbnailStudioPlugin);
