#START-FILE-LIST

start_sources()
sources_platform(ALL)
add_sources("NoUberFile"
	SOURCE_GROUP "Root"
		"StdAfx.h"
		"StdAfx.cpp"
		"Plugin.h"
		"Plugin.cpp"
		"CryAgentThumbnailExtension.h"
		"CryAgentThumbnailExtension.cpp"
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
end_sources()

#END-FILE-LIST

CryEditorPlugin(AssetThumbnailStudio PCH "StdAfx.cpp" SOLUTION_FOLDER "Sandbox/Plugins")

target_include_directories(${THIS_PROJECT} PRIVATE
	"${CRYENGINE_DIR}/Code/CryPlugins/CryAgentSDK/Interface"
	"${CRYENGINE_DIR}/Code/CryPlugins/CryAgentSDK/SDK"
)

target_link_libraries(${THIS_PROJECT} PRIVATE CryAgentSDK)
