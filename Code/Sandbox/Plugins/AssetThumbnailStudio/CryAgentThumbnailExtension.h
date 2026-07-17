// Copyright 2026
#pragma once

#include <ExtensionAPI.h>

namespace CryAgentSDK
{
struct ICryAgentExtensionHost;
}

namespace AssetThumbnailStudio
{

class CThumbnailGenerationService;

class CCryAgentThumbnailExtension final : public CryAgentSDK::IExtensionModuleV1
{
public:
	explicit CCryAgentThumbnailExtension(CThumbnailGenerationService& service);
	~CCryAgentThumbnailExtension() override;

	bool Attach();
	void Detach();
	bool IsAttached() const { return m_pHost != nullptr; }

	CryAgentSDK::SExtensionInfoV1 ExtensionInfo() const override;
	bool RegisterExtension(CryAgentSDK::IExtensionRegistrarV1& registrar, CryAgentSDK::ISdkContext& context,
		std::string& error) override;

private:
	CThumbnailGenerationService&          m_service;
	CryAgentSDK::ICryAgentExtensionHost* m_pHost = nullptr;
	bool                                  m_hostUnavailableLogged = false;
	bool                                  m_registrationFailed = false;
};

} // namespace AssetThumbnailStudio
