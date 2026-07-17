// Copyright 2026
#include "StdAfx.h"
#include "ThumbnailJobQueue.h"
#include "ThumbnailStageRenderer.h"

#include <AssetSystem/Asset.h>
#include <Notifications/NotificationCenter.h>

#include <QObject>
#include <QTimer>

namespace AssetThumbnailStudio
{
namespace
{

constexpr int kRendererLingerMilliseconds = 10000;

} // namespace

CThumbnailJobQueue::CThumbnailJobQueue()
	: m_pProcessTimer(new QTimer())
	, m_pLingerTimer(new QTimer())
{
	m_pProcessTimer->setInterval(0);
	QObject::connect(m_pProcessTimer.get(), &QTimer::timeout, [this]() { ProcessNext(); });

	m_pLingerTimer->setSingleShot(true);
	m_pLingerTimer->setInterval(kRendererLingerMilliseconds);
	QObject::connect(m_pLingerTimer.get(), &QTimer::timeout, [this]()
	{
		DestroyRenderer("linger_expired");
	});
}

CThumbnailJobQueue::~CThumbnailJobQueue()
{
	Shutdown();
}

void CThumbnailJobQueue::Enqueue(const std::vector<CAssetPtr>& assets)
{
	if (assets.empty())
	{
		return;
	}

	if (m_state != EState::Working)
	{
		m_batchTotal = 0;
		m_batchCompleted = 0;
	}

	const size_t requested = assets.size();
	size_t accepted = 0;
	size_t skipped = 0;
	for (const CAssetPtr& pAsset : assets)
	{
		if (!pAsset)
		{
			++skipped;
			continue;
		}

		const std::string dedupKey = pAsset->GetMetadataFile().c_str();
		if (dedupKey.empty())
		{
			++skipped;
			continue;
		}

		if (!m_pendingOrActiveKeys.insert(dedupKey).second)
		{
			++skipped;
			CryLog("[AssetThumbnailStudio] ui_queue_duplicate_skipped metadata=%s reason=pending_or_active",
				dedupKey.c_str());
			continue;
		}

		SQueuedAsset queued;
		queued.pAsset = pAsset;
		queued.dedupKey = dedupKey;
		m_pending.push_back(std::move(queued));
		++accepted;
	}

	if (accepted == 0)
	{
		CryLog("[AssetThumbnailStudio] ui_queue_enqueue requested=%llu accepted=0 skipped=%llu pending=%llu total=%llu state=%s",
			static_cast<unsigned long long>(requested), static_cast<unsigned long long>(skipped),
			static_cast<unsigned long long>(m_pending.size()), static_cast<unsigned long long>(m_batchTotal), StateName());
		return;
	}

	m_batchTotal += accepted;
	if (m_state == EState::Linger)
	{
		m_pLingerTimer->stop();
		m_state = EState::Working;
		CryLog("[AssetThumbnailStudio] ui_queue_renderer_reused generation=%llu lingerCancelled=true",
			static_cast<unsigned long long>(m_rendererGeneration));
	}
	else if (m_state == EState::Idle)
	{
		m_pRenderer.reset(new CThumbnailStageRenderer());
		if (!m_pRenderer->IsReady())
		{
			CryWarning(VALIDATOR_MODULE_EDITOR, VALIDATOR_WARNING,
				"[AssetThumbnailStudio] UI queue could not create the thumbnail renderer; discarded %llu queued item(s).",
				static_cast<unsigned long long>(m_pending.size()));
			m_pRenderer.reset();
			m_pending.clear();
			m_pendingOrActiveKeys.clear();
			m_batchTotal = 0;
			m_batchCompleted = 0;
			return;
		}
		m_state = EState::Working;
		++m_rendererGeneration;
		CryLog("[AssetThumbnailStudio] ui_queue_renderer_created generation=%llu",
			static_cast<unsigned long long>(m_rendererGeneration));
	}

	if (!m_pProgress)
	{
		m_pProgress.reset(new CProgressNotification(
			QObject::tr("Generating thumbnails (Studio)"), QString(), true));
	}
	m_pProgress->SetMessage(QObject::tr("%1 queued").arg(m_pending.size()));
	m_pProgress->SetProgress(m_batchTotal > 0
		? static_cast<float>(m_batchCompleted) / static_cast<float>(m_batchTotal)
		: 0.0f);

	CryLog("[AssetThumbnailStudio] ui_queue_enqueue requested=%llu accepted=%llu skipped=%llu pending=%llu total=%llu state=%s",
		static_cast<unsigned long long>(requested), static_cast<unsigned long long>(accepted),
		static_cast<unsigned long long>(skipped), static_cast<unsigned long long>(m_pending.size()),
		static_cast<unsigned long long>(m_batchTotal), StateName());
	if (!m_pProcessTimer->isActive())
	{
		m_pProcessTimer->start();
	}
}

void CThumbnailJobQueue::ProcessNext()
{
	IEditor* const pEditor = GetIEditor();
	if (!pEditor || pEditor->IsMainFrameClosing())
	{
		AbortForEditorShutdown();
		return;
	}

	if (m_pending.empty())
	{
		FinishBatch();
		return;
	}

	SQueuedAsset queued = std::move(m_pending.front());
	m_pending.pop_front();
	m_activeKey = queued.dedupKey;
	if (m_pProgress)
	{
		m_pProgress->SetMessage(QObject::tr("for asset '%1'").arg(queued.dedupKey.c_str()));
	}

	CryLog("[AssetThumbnailStudio] ui_queue_item_begin metadata=%s completed=%llu total=%llu",
		queued.dedupKey.c_str(), static_cast<unsigned long long>(m_batchCompleted),
		static_cast<unsigned long long>(m_batchTotal));
	const bool succeeded = m_pRenderer && m_pRenderer->RenderAssetThumbnail(queued.pAsset);
	if (succeeded)
	{
		queued.pAsset->InvalidateThumbnail();
	}
	else
	{
		CryWarning(VALIDATOR_MODULE_EDITOR, VALIDATOR_WARNING,
			"[AssetThumbnailStudio] Thumbnail generation failed for '%s'.", queued.dedupKey.c_str());
	}

	m_pendingOrActiveKeys.erase(queued.dedupKey);
	m_activeKey.clear();
	++m_batchCompleted;
	if (m_pProgress && m_batchTotal > 0)
	{
		m_pProgress->SetProgress(static_cast<float>(m_batchCompleted) / static_cast<float>(m_batchTotal));
	}
	CryLog("[AssetThumbnailStudio] ui_queue_item_end metadata=%s ok=%s completed=%llu total=%llu remaining=%llu",
		queued.dedupKey.c_str(), succeeded ? "true" : "false",
		static_cast<unsigned long long>(m_batchCompleted), static_cast<unsigned long long>(m_batchTotal),
		static_cast<unsigned long long>(m_pending.size()));

	if (m_pending.empty())
	{
		FinishBatch();
	}
}

void CThumbnailJobQueue::FinishBatch()
{
	if (m_state != EState::Working)
	{
		return;
	}

	m_pProcessTimer->stop();
	m_pProgress.reset();
	m_pendingOrActiveKeys.clear();
	m_activeKey.clear();
	m_state = EState::Linger;
	CryLog("[AssetThumbnailStudio] ui_queue_linger_begin generation=%llu completed=%llu total=%llu timeoutMs=%d",
		static_cast<unsigned long long>(m_rendererGeneration), static_cast<unsigned long long>(m_batchCompleted),
		static_cast<unsigned long long>(m_batchTotal), kRendererLingerMilliseconds);
	m_pLingerTimer->start();
}

void CThumbnailJobQueue::AbortForEditorShutdown()
{
	const size_t abandoned = m_pending.size() + (m_activeKey.empty() ? 0 : 1);
	CryLog("[AssetThumbnailStudio] ui_queue_shutdown_abort abandoned=%llu contextWillBeDestroyed=true",
		static_cast<unsigned long long>(abandoned));
	Shutdown();
}

void CThumbnailJobQueue::Shutdown()
{
	if (m_pProcessTimer)
	{
		m_pProcessTimer->stop();
	}
	if (m_pLingerTimer)
	{
		m_pLingerTimer->stop();
	}
	m_pending.clear();
	m_pendingOrActiveKeys.clear();
	m_activeKey.clear();
	m_pProgress.reset();
	DestroyRenderer("shutdown");
	m_batchTotal = 0;
	m_batchCompleted = 0;
}

void CThumbnailJobQueue::DestroyRenderer(const char* const szReason)
{
	const bool hadRenderer = static_cast<bool>(m_pRenderer);
	m_pRenderer.reset();
	m_state = EState::Idle;
	if (hadRenderer)
	{
		CryLog("[AssetThumbnailStudio] ui_queue_renderer_destroyed generation=%llu reason=%s",
			static_cast<unsigned long long>(m_rendererGeneration), szReason ? szReason : "unknown");
	}
}

bool CThumbnailJobQueue::IsGenerating() const
{
	return m_state == EState::Working;
}

const char* CThumbnailJobQueue::StateName() const
{
	switch (m_state)
	{
	case EState::Working:
		return "working";
	case EState::Linger:
		return "linger";
	case EState::Idle:
	default:
		return "idle";
	}
}

} // namespace AssetThumbnailStudio
