// Copyright 2026
#pragma once

#include <AssetSystem/Asset.h>

#include <deque>
#include <cstdint>
#include <memory>
#include <set>
#include <string>
#include <vector>

class CProgressNotification;
class QTimer;

namespace AssetThumbnailStudio
{

class CThumbnailStageRenderer;

class CThumbnailJobQueue final
{
public:
	CThumbnailJobQueue();
	~CThumbnailJobQueue();

	void Enqueue(const std::vector<CAssetPtr>& assets);
	void Shutdown();

	bool IsGenerating() const;

private:
	enum class EState
	{
		Idle,
		Working,
		Linger
	};

	struct SQueuedAsset
	{
		CAssetPtr   pAsset;
		std::string dedupKey;
	};

	void ProcessNext();
	void FinishBatch();
	void AbortForEditorShutdown();
	void DestroyRenderer(const char* szReason);
	const char* StateName() const;

private:
	std::unique_ptr<QTimer>                  m_pProcessTimer;
	std::unique_ptr<QTimer>                  m_pLingerTimer;
	std::unique_ptr<CThumbnailStageRenderer> m_pRenderer;
	std::unique_ptr<CProgressNotification>   m_pProgress;
	std::deque<SQueuedAsset>                 m_pending;
	std::set<std::string>                    m_pendingOrActiveKeys;
	std::string                              m_activeKey;
	EState                                   m_state = EState::Idle;
	size_t                                   m_batchTotal = 0;
	size_t                                   m_batchCompleted = 0;
	uint64_t                                 m_rendererGeneration = 0;
};

} // namespace AssetThumbnailStudio
