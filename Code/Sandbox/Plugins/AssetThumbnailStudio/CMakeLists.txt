option(ASSET_THUMBNAIL_STUDIO_WITH_CRYAGENTSDK
	"Build AssetThumbnailStudio's optional CryAgentSDK command adapter"
	OFF)

#START-FILE-LIST

start_sources()
sources_platform(ALL)
add_sources("NoUberFile"
	SOURCE_GROUP "Root"
		"StdAfx.h"
		"StdAfx.cpp"
		"Plugin.h"
		"Plugin.cpp"
		"ThumbnailGenerationService.h"
		"ThumbnailGenerationService.cpp"
		"ThumbnailJobQueue.h"
		"ThumbnailJobQueue.cpp"
	SOURCE_GROUP "Renderer"
		"StudioAssets.h"
		"MitchellDownscaler.h"
		"MitchellDownscaler.cpp"
		"ThumbnailStageRenderer.h"
		"ThumbnailStageRenderer.cpp"
)
if (ASSET_THUMBNAIL_STUDIO_WITH_CRYAGENTSDK)
	add_sources("NoUberFile"
		SOURCE_GROUP "CryAgentSDK Adapter"
			"CryAgentThumbnailExtension.h"
			"CryAgentThumbnailExtension.cpp"
	)
endif()
end_sources()

#END-FILE-LIST

CryEditorPlugin(AssetThumbnailStudio PCH "StdAfx.cpp" SOLUTION_FOLDER "Sandbox/Plugins")

if (ASSET_THUMBNAIL_STUDIO_WITH_CRYAGENTSDK)
	target_compile_definitions(${THIS_PROJECT} PRIVATE ASSET_THUMBNAIL_STUDIO_WITH_CRYAGENTSDK=1)
	target_include_directories(${THIS_PROJECT} PRIVATE
		"${CRYENGINE_DIR}/Code/CryPlugins/CryAgentSDK/Interface"
		"${CRYENGINE_DIR}/Code/CryPlugins/CryAgentSDK/SDK"
	)
	target_link_libraries(${THIS_PROJECT} PRIVATE CryAgentSDK)
else()
	target_compile_definitions(${THIS_PROJECT} PRIVATE ASSET_THUMBNAIL_STUDIO_WITH_CRYAGENTSDK=0)
endif()
