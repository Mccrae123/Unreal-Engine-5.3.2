// Copyright Epic Games, Inc. All Rights Reserved.


#include "CoreMinimal.h"
#include "GlobalEditorNotification.h"
#include "ShaderCompiler.h"
#include "Widgets/Notifications/SNotificationList.h"

#define LOCTEXT_NAMESPACE "ShaderCompiler"

/** Notification class for asynchronous shader compiling. */
class FShaderCompilingNotificationImpl : public FGlobalEditorNotification
{
protected:
	/** FGlobalEditorNotification interface */
	virtual void OverrideNotifcationInfo(struct FNotificationInfo& OutInfo) const override;
	virtual bool ShouldShowNotification(const bool bIsNotificationAlreadyActive) const override;
	virtual void SetNotificationText(const TSharedPtr<SNotificationItem>& InNotificationItem) const override;
};

/** Global notification object. */
FShaderCompilingNotificationImpl GShaderCompilingNotification;

void FShaderCompilingNotificationImpl::OverrideNotifcationInfo(struct FNotificationInfo& OutInfo) const
{
	FNotificationButtonInfo CancelButton(
		LOCTEXT("ShaderCompilingNotificationCancel", "Cancel"),
		LOCTEXT("ShaderCompilingNotificationTooltip", "Cancels compiling all queued shaders"),
		FSimpleDelegate::CreateLambda([]
		{
			if (GShaderCompilingManager)
			{
				GShaderCompilingManager->CancelAllCompilations();
			}
		})
	);
	OutInfo.ButtonDetails.Add(CancelButton);
}

bool FShaderCompilingNotificationImpl::ShouldShowNotification(const bool bIsNotificationAlreadyActive) const
{
	// ShouldDisplayCompilingNotification is more a hint, and may start returning false while there's still work being done
	// If we're already showing the notification, we should continue to do so until all the jobs have finished
	return GShaderCompilingManager->ShouldDisplayCompilingNotification() || (bIsNotificationAlreadyActive && GShaderCompilingManager->IsCompiling());
}

void FShaderCompilingNotificationImpl::SetNotificationText(const TSharedPtr<SNotificationItem>& InNotificationItem) const
{
	if (GShaderCompilingManager->IsCompiling())
	{
		FFormatNamedArguments Args;
		Args.Add(TEXT("ShaderJobs"), FText::AsNumber(GShaderCompilingManager->GetNumRemainingJobs()));
		const FText ProgressMessage = FText::Format(NSLOCTEXT("ShaderCompile", "ShaderCompileInProgressFormat", "Compiling Shaders ({ShaderJobs})"), Args);

		InNotificationItem->SetText(ProgressMessage);
	}
}
#undef LOCTEXT_NAMESPACE
