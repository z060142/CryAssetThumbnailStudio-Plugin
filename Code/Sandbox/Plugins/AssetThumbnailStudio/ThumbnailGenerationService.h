// Copyright 2026
#pragma once

#include <AssetSystem/Asset.h>

#include <deque>
#include <memory>
#include <string>
#include <vector>

namespace AssetThumbnailStudio
{

class CThumbnailJobQueue;

struct SThumbnailGenerationResponse
{
	bool        ok = false;
	std::string json;
};

class CThumbnailGenerationService final
{
public:
	CThumbnailGenerationService();
	~CThumbnailGenerationService();

	SThumbnailGenerationResponse GenerateCryAgentRequest(const std::string& rawJson);
	void GenerateForUi(const std::vector<CAssetPtr>& assets);
	void ShutdownUiQueue();

private:
	struct SCachedRequest
	{
		std::string requestId;
		std::string payloadKey;
		std::string responseJson;
		bool        ok = false;
	};

	SThumbnailGenerationResponse Generate(const std::string& requestId, const std::vector<std::string>& metadataPaths);
	void CacheCompleted(const std::string& requestId, const std::string& payloadKey, const SThumbnailGenerationResponse& response);

	std::deque<SCachedRequest>          m_completedRequests;
	bool                                m_busy = false;
	std::unique_ptr<CThumbnailJobQueue> m_pUiQueue;
};

} // namespace AssetThumbnailStudio
