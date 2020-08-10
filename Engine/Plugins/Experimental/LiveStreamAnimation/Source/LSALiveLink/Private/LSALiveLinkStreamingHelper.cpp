// Copyright Epic Games, Inc. All Rights Reserverd.

#include "LSALiveLinkStreamingHelper.h"
#include "LSALiveLinkLog.h"
#include "LSALiveLinkDataHandler.h"
#include "LSALiveLinkSettings.h"
#include "LSALiveLinkPacket.h"
#include "LSALiveLinkSource.h"
#include "LSALiveLinkSkelMeshSource.h"
#include "Serialization/MemoryWriter.h"
#include "LiveStreamAnimationRole.h"

#include "Animation/Skeleton.h"

#include "Roles/LiveLinkAnimationTypes.h"
#include "Roles/LiveLinkAnimationRole.h"
#include "ILiveLinkClient.h"
#include "LiveLinkTypes.h"
#include "Serialization/MemoryReader.h"
#include "Features/IModularFeatures.h"
#include "CoreGlobals.h"

#include "UObject/CoreNet.h"

#include "Algo/Sort.h"

bool FLSALiveLinkStreamingHelper::FLiveLinkTrackedSubject::ReceivedFrameData(const FLiveLinkAnimationFrameData& AnimationData, FLiveLinkAnimationFrameData& OutAnimationData) const
{
	OutAnimationData = AnimationData;
	if (BoneTranslations.Num() > 0)
	{
		TArray<FTransform> TranslatedTransforms;
		TranslatedTransforms.Reset(BoneTranslations.Num());

		for (int32 TranslationIndex : BoneTranslations)
		{
			TranslatedTransforms.Add(AnimationData.Transforms[TranslationIndex]);
		}

		OutAnimationData.Transforms = MoveTemp(TranslatedTransforms);
	}

	return true;
}

bool FLSALiveLinkStreamingHelper::FLiveLinkTrackedSubject::ReceivedStaticData(const FLiveLinkSkeletonStaticData& SkeletonData)
{
	struct FBoneRemapInfo
	{
		int32 BonesToUseIndex;
		int32 RemappedSkeletonIndex;
		int32 RealSkeletonIndex;
	};

	// TODO: This will cause us to crash if the LiveLinkSubject is missing any of the bones we specify in BonesToUse.
	//			We should add some code that detects that, and pads the transforms with either identity transforms
	//			**or** some code that attempts to find the bone from the UE Skeleton and grabs its ref pose (if possible).

	if (TranslationProfile.IsSet())
	{
		const FLSALiveLinkTranslationProfile& UseProfile = TranslationProfile.GetValue();
		if (UseProfile.BonesToUse.Num() > 0)
		{
			TArray<FBoneRemapInfo> BoneRemapArray;
			BoneRemapArray.Reserve(SkeletonData.BoneNames.Num());

			TArray<int32> BoneParents;
			BoneParents.Reserve(UseProfile.BonesToUse.Num());

			TArray<int32> RemovedBonesAtIndex;
			RemovedBonesAtIndex.SetNumUninitialized(SkeletonData.BoneNames.Num());

			TBitArray<> UseBones(false, SkeletonData.BoneNames.Num());

			// Shift Counter / RemovesBonesAtIndex is just a convenience.
			// We could just count the number of unset bits between the start of the
			// UseBones bitfield and the index of the bone we're checking, but that
			// seems pretty inefficient.
			int32 ShiftCounter = 0;

			// Maybe converting BoneNames to a Set would be faster, but the count is always going to be
			// low so it's probably fine.
			// Also, this is only going to happen when we receive skeleton data, which will almost always
			// only happen once per subject when we initially connect.

			// First, go ahead and filter out the bones we aren't going to use from the skeleton.
			// Track the bones in our remap array so we can shuffle them later, and track
			// offsets so we can adjust parent indices without searching again.
			for (int32 i = 0; i < SkeletonData.BoneNames.Num(); ++i)
			{
				const int32 BoneNameIndex = UseProfile.BonesToUse.Find(SkeletonData.BoneNames[i]);
				if (INDEX_NONE != BoneNameIndex)
				{
					UseBones[i] = true;
					BoneRemapArray.Add({ BoneNameIndex, BoneRemapArray.Num(), i });
				}
				else
				{
					++ShiftCounter;
				}

				RemovedBonesAtIndex[i] = ShiftCounter;
			}

			// Next, fixup our parent indices.
			// Bones and parents should still be in the same *order* as the incoming skeleton data
			// at this point, but we will have entries missing. So, in order to find the appropriate
			// new parent bone, we will search until we find an ancestor that was included, and then
			// shift its index to compensate for bones that were removed.
			for (TConstSetBitIterator<> ConstIt(UseBones); ConstIt; ++ConstIt)
			{
				int32 ParentIndex = ConstIt.GetIndex();
				while (true)
				{
					ParentIndex = SkeletonData.BoneParents[ParentIndex];

					if (ParentIndex == INDEX_NONE)
					{
						break;
					}

					// We found a bone that was enabled that is also an ancestor.
					// Go ahead and fix up its index.
					if (UseBones[ParentIndex])
					{
						ParentIndex -= RemovedBonesAtIndex[ParentIndex];
						break;
					}
				}

				BoneParents.Add(ParentIndex);
			}

			// Finally, we need to shuffle our bones around and create a translation from the
			// incoming skeleton to the bones we want.

			BoneTranslations.SetNumUninitialized(BoneRemapArray.Num());
			LastKnownSkeleton.BoneNames.SetNumUninitialized(BoneRemapArray.Num());
			LastKnownSkeleton.BoneParents.SetNumUninitialized(BoneRemapArray.Num());
				
			for (const FBoneRemapInfo& RemapInfo : BoneRemapArray)
			{
				const int32 BonesToUseIndex = RemapInfo.BonesToUseIndex;
				BoneTranslations[BonesToUseIndex] = RemapInfo.RealSkeletonIndex;
				LastKnownSkeleton.BoneNames[BonesToUseIndex] = UseProfile.BonesToUse[BonesToUseIndex];
				
				const int32 OldParentIndex = BoneParents[RemapInfo.RemappedSkeletonIndex];
				LastKnownSkeleton.BoneParents[BonesToUseIndex] = (INDEX_NONE == OldParentIndex) ? INDEX_NONE : BoneRemapArray[OldParentIndex].BonesToUseIndex;
			}

			return true;
		}
	}

	LastKnownSkeleton = SkeletonData;
	return true;
}

FString FLSALiveLinkStreamingHelper::FLiveLinkTrackedSubject::ToString() const
{
	return FString::Printf(TEXT("LiveLinkSubject = %s, SubjectHandle = %s"),
		*LiveLinkSubject.ToString(), *SubjectHandle.ToString());
}

FLSALiveLinkStreamingHelper::FLiveLinkTrackedSubject FLSALiveLinkStreamingHelper::FLiveLinkTrackedSubject::CreateFromReceivedPacket(
	FLiveLinkSubjectName InLiveLinkSubject,
	FLiveStreamAnimationHandle InSubjectHandle,
	const FLiveLinkSkeletonStaticData& InSkeleton)
{
	FLiveLinkTrackedSubject NewSubject;
	NewSubject.LiveLinkSubject = InLiveLinkSubject;
	NewSubject.SubjectHandle = InSubjectHandle;
	NewSubject.LastKnownSkeleton = InSkeleton;

	return NewSubject;
}

FLSALiveLinkStreamingHelper::FLiveLinkTrackedSubject FLSALiveLinkStreamingHelper::FLiveLinkTrackedSubject::CreateFromTrackingRequest(
	FLiveLinkSubjectName InLiveLinkSubject,
	FLiveStreamAnimationHandle InSubjectHandle,
	FLSALiveLinkSourceOptions InOptions,
	FLiveStreamAnimationHandle InTranslationHandle,
	FDelegateHandle InStaticDataReceivedHandle,
	FDelegateHandle InFrameDataReceivedHandle)
{
	FLiveLinkTrackedSubject NewSubject;
	NewSubject.LiveLinkSubject = InLiveLinkSubject;
	NewSubject.SubjectHandle = InSubjectHandle;
	NewSubject.Options = InOptions;
	NewSubject.TranslationHandle = InTranslationHandle;
	NewSubject.StaticDataReceivedHandle = InStaticDataReceivedHandle;
	NewSubject.FrameDataReceivedHandle = InFrameDataReceivedHandle;

	if (InTranslationHandle.IsValid())
	{
		if (const ULSALiveLinkFrameTranslator* Translator = ULSALiveLinkSettings::GetFrameTranslator())
		{
			if (const FLSALiveLinkTranslationProfile* FoundTranslationProfile = Translator->GetTranslationProfile(InTranslationHandle))
			{
				NewSubject.TranslationProfile = *FoundTranslationProfile;
			}
		}
	}

	return NewSubject;
}

FLSALiveLinkStreamingHelper::FLSALiveLinkStreamingHelper(ULSALiveLinkDataHandler& InDataHandler)
	: DataHandler(InDataHandler)
	, OnFrameTranslatorChangedHandle(ULSALiveLinkSettings::AddFrameTranslatorChangedCallback(FSimpleMulticastDelegate::FDelegate::CreateRaw(this, &FLSALiveLinkStreamingHelper::OnFrameTranslatorChanged)))
{
	if (ELiveStreamAnimationRole::Processor == DataHandler.GetRole())
	{
		StartProcessingPackets();
	}
}

FLSALiveLinkStreamingHelper::~FLSALiveLinkStreamingHelper()
{
	RemoveAllSubjects();
	StopProcessingPackets();

	if (SkelMeshToLiveLinkSource.IsValid())
	{
		if (!IsEngineExitRequested())
		{
			if (ILiveLinkClient* LiveLinkClient = GetLiveLinkClient())
			{
				LiveLinkClient->RemoveSource(SkelMeshToLiveLinkSource);
			}
		}
	}

	ULSALiveLinkSettings::RemoveFrameTranslatorChangedCallback(OnFrameTranslatorChangedHandle);
}

void FLSALiveLinkStreamingHelper::OnPacketReceived(const TArrayView<const uint8> PacketData)
{
	// TODO: We could probably add a way to peak Live Link Packet Type
	//			and just ignore Animation updates if we aren't going to
	//			process them, since we don't need to keep those records
	//			up to date.
	//			This could help perf, especially since non-animation updates
	//			would be rare.

	FMemoryReaderView Reader(PacketData);
	TUniquePtr<FLSALiveLinkPacket> LiveLinkPacketUniquePtr(FLSALiveLinkPacket::ReadFromStream(Reader));

	if (FLSALiveLinkPacket* LiveLinkPacket = LiveLinkPacketUniquePtr.Get())
	{
		if (FLSALiveLinkSource* LocalLiveLinkSource = LiveLinkSource.Get())
		{
			LocalLiveLinkSource->HandlePacket(MoveTemp(*LiveLinkPacket));
		}

		const FLiveStreamAnimationHandle SubjectHandle = LiveLinkPacket->GetSubjectHandle();

		// Now, update our records.
		switch (LiveLinkPacket->GetPacketType())
		{
		case ELSALiveLinkPacketType::RemoveSubject:
			TrackedSubjects.Remove(SubjectHandle);
			break;

		case ELSALiveLinkPacketType::AddOrUpdateSubject:
			{
				const FLSALiveLinkAddOrUpdateSubjectPacket& CastedPacket = static_cast<const FLSALiveLinkAddOrUpdateSubjectPacket&>(*LiveLinkPacket);
				if (FLiveLinkTrackedSubject* FoundSubject = TrackedSubjects.Find(SubjectHandle))
				{
					FoundSubject->LastKnownSkeleton = CastedPacket.GetStaticData();
				}
				else
				{
					FLiveLinkTrackedSubject NewSubject = FLiveLinkTrackedSubject::CreateFromReceivedPacket(
						SubjectHandle.GetName(),			// For processors and proxies, we don't care about the originating Live Link name.
															// Instead we use the associated handle name.
						SubjectHandle,
						CastedPacket.GetStaticData());

					TrackedSubjects.Add(SubjectHandle, NewSubject);
				}
			}

			break;
				
		default:
			break;
		}
	}
	else
	{
		UE_LOG(LogLSALiveLink, Warning, TEXT("FLSALiveLinkStreamingHelper::HandleLiveLinkPacket: Received invalid Live Link Packet!"));
	}
}

void FLSALiveLinkStreamingHelper::OnAnimationRoleChanged(ELiveStreamAnimationRole NewRole)
{
	if (ELiveStreamAnimationRole::Processor == NewRole)
	{
		StartProcessingPackets();
	}
	else
	{
		StopProcessingPackets();
	}
}

bool FLSALiveLinkStreamingHelper::StartTrackingLiveLinkSubject(
	const FName LiveLinkSubject,
	const FLiveStreamAnimationHandle SubjectHandle,
	const FLSALiveLinkSourceOptions Options,
	const FLiveStreamAnimationHandle TranslationHandle)
{
	if (LiveLinkSubject == NAME_None)
	{
		UE_LOG(LogLSALiveLink, Warning, TEXT("FLSALiveLinkStreamingHelper::StartTrackingSubject: Invalid LiveLinkSubject."));
		return false;
	}

	if (!SubjectHandle.IsValid())
	{
		UE_LOG(LogLSALiveLink, Warning, TEXT("FLSALiveLinkStreamingHelper::StartTrackingSubject: Invalid SubjectHandle."));
		return false;
	}

	if (!Options.IsValid())
	{
		UE_LOG(LogLSALiveLink, Warning, TEXT("FLSALiveLinkStreamingHelper::StartTrackingSubject: Invalid Options."));
		return false;
	}

	FLiveLinkSubjectName LiveLinkSubjectName(LiveLinkSubject);
	ILiveLinkClient* LiveLinkClient = GetLiveLinkClient();
	if (!LiveLinkClient)
	{
		UE_LOG(LogLSALiveLink, Warning, TEXT("FLSALiveLinkStreamingHelper::StartTrackingSubject: Unable to get LiveLinkClient."));
		return false;
	}

	if (FLiveLinkTrackedSubject* ExistingSubject = TrackedSubjects.Find(SubjectHandle))
	{
		UE_LOG(LogLSALiveLink, Warning, TEXT("FLSALiveLinkStreamingHelper::StartTrackingSubject: Subject is already tracked. ExistingSubject = (%s)"),
			*ExistingSubject->ToString());

		const FLiveLinkSubjectName RegisteredSubjectName(SubjectHandle.GetName());
		if (LiveLinkClient->IsSubjectValid(RegisteredSubjectName))
		{
			return ExistingSubject->LiveLinkSubject == LiveLinkSubject;
		}
		else
		{
			UE_LOG(LogLSALiveLink, Warning, TEXT("FLSALiveLinkStreamingHelper::StartTrackingSubject: Subject was tracked, but removed from Live Link. Reregistering. ExistingSubject = (%s)"),
				*ExistingSubject->ToString());
		}
	}

	if (!LiveLinkClient->HasSourceBeenAdded(LiveLinkSource))
	{
		UE_LOG(LogLSALiveLink, Warning, TEXT("FLSALiveLinkStreamingHelper::StartTrackingSubject: Live Stream Animation Live Link Source was removed from Live Link! Previously tracked subjects may not be valid anymore."));
		LiveLinkClient->AddSource(LiveLinkSource);
	}

	FDelegateHandle StaticDataReceivedHandle;
	FOnLiveLinkSubjectStaticDataAdded::FDelegate OnStaticDataReceived;
	OnStaticDataReceived.BindRaw(this, &FLSALiveLinkStreamingHelper::ReceivedStaticData, SubjectHandle);

	FDelegateHandle FrameDataReceivedHandle;
	FOnLiveLinkSubjectFrameDataAdded::FDelegate OnFrameDataReceived;
	OnFrameDataReceived.BindRaw(this, &FLSALiveLinkStreamingHelper::ReceivedFrameData, SubjectHandle);

	TSubclassOf<ULiveLinkRole> SubjectRole;
	FLiveLinkStaticDataStruct StaticData;

	bool bSuccess = false;

	const bool bWasRegistered = LiveLinkClient->RegisterForSubjectFrames(
		LiveLinkSubjectName,
		OnStaticDataReceived,
		OnFrameDataReceived,
		StaticDataReceivedHandle,
		FrameDataReceivedHandle,
		SubjectRole,
		&StaticData);

	FLiveLinkTrackedSubject TrackedSubject = FLiveLinkTrackedSubject::CreateFromTrackingRequest(
		LiveLinkSubjectName,
		SubjectHandle,
		Options,
		TranslationHandle,
		StaticDataReceivedHandle,
		FrameDataReceivedHandle);

	if (bWasRegistered)
	{
		if (!SubjectRole->IsChildOf(ULiveLinkAnimationRole::StaticClass()))
		{
			UE_LOG(LogLSALiveLink, Warning, TEXT("FLSALiveLinkStreamingHelper::StartTrackingSubject: Subject had invalid role, subject won't be sent. Subject = (%s), Role = %s"),
				*TrackedSubject.ToString(), *GetPathNameSafe(SubjectRole.Get()));
		}
		else if (!StaticData.IsValid())
		{
			UE_LOG(LogLSALiveLink, Warning, TEXT("FLSALiveLinkStreamingHelper::StartTrackingSubject: Subject didn't have static data. Subject will be sent later, when static data is received. Subject = (%s)"),
				*TrackedSubject.ToString());

			bSuccess = true;
			TrackedSubjects.Add(SubjectHandle, TrackedSubject);
		}
		else
		{
			if (const FLiveLinkSkeletonStaticData* SkeletonDataPtr = StaticData.Cast<FLiveLinkSkeletonStaticData>())
			{
				TrackedSubject.ReceivedStaticData(*SkeletonDataPtr);
			}

			if (SendPacketToServer(CreateAddOrUpdateSubjectPacket(TrackedSubject)))
			{
				bSuccess = true;
				TrackedSubjects.Add(SubjectHandle, TrackedSubject);
			}
			else
			{
				UE_LOG(LogLSALiveLink, Warning, TEXT("FLSALiveLinkStreamingHelper::StartTrackingSubject: Failed to send add subject packet. Subject = (%s)"),
					*TrackedSubject.ToString());
			}
		}

		if (!bSuccess)
		{
			LiveLinkClient->UnregisterSubjectFramesHandle(TrackedSubject.LiveLinkSubject, TrackedSubject.StaticDataReceivedHandle, TrackedSubject.FrameDataReceivedHandle);
		}
	}
	else
	{
		UE_LOG(LogLSALiveLink, Warning, TEXT("FLSALiveLinkStreamingHelper::StartTrackingSubject: Failed to register subject. Subject = (%s)"),
			*TrackedSubject.ToString());
	}

	return bSuccess;
}

void FLSALiveLinkStreamingHelper::StopTrackingLiveLinkSubject(const FLiveStreamAnimationHandle SubjectHandle)
{
	if (const FLiveLinkTrackedSubject* TrackedSubject = TrackedSubjects.Find(SubjectHandle))
	{
		if (ILiveLinkClient* LiveLinkClient = GetLiveLinkClient())
		{
			LiveLinkClient->UnregisterSubjectFramesHandle(TrackedSubject->LiveLinkSubject, TrackedSubject->StaticDataReceivedHandle, TrackedSubject->FrameDataReceivedHandle);
			if (!SendPacketToServer(CreateRemoveSubjectPacket(*TrackedSubject)))
			{
				UE_LOG(LogLSALiveLink, Warning, TEXT("FLSALiveLinkStreamingHelper::StopTrackingSubject: Failed to send remove packet to server. Subject = (%s)"),
					*TrackedSubject->ToString());
			}
		}

		TrackedSubjects.Remove(SubjectHandle);
	}
	else
	{
		UE_LOG(LogLSALiveLink, Warning, TEXT("FLSALiveLinkStreamingHelper::StopTrackingSubject: Unable to find subject. SubjectHandle = %s"),
			*SubjectHandle.ToString());
	}
}

void FLSALiveLinkStreamingHelper::StartProcessingPackets()
{
	if (!LiveLinkSource.IsValid())
	{
		if (ILiveLinkClient* LiveLinkClient = GetLiveLinkClient())
		{
			LiveLinkSource = MakeShared<FLSALiveLinkSource>(ULSALiveLinkSettings::GetFrameTranslator());
			LiveLinkClient->AddSource(StaticCastSharedPtr<ILiveLinkSource>(LiveLinkSource));

			// If we've already received data, go ahead and get our Source back up to date.
			for (auto It = TrackedSubjects.CreateIterator(); It; ++It)
			{
				const FLiveLinkTrackedSubject& TrackedSubject = It.Value();

				TUniquePtr<FLSALiveLinkPacket> Packet = FLSALiveLinkAddOrUpdateSubjectPacket::CreatePacket(
					TrackedSubject.SubjectHandle,
					FLiveLinkSkeletonStaticData(TrackedSubject.LastKnownSkeleton));

				if (Packet.IsValid())
				{
					LiveLinkSource->HandlePacket(MoveTemp(*Packet));
				}
			}
		}
	}
}

void FLSALiveLinkStreamingHelper::StopProcessingPackets()
{
	if (!IsEngineExitRequested())
	{
		if (LiveLinkSource.IsValid())
		{
			if (ILiveLinkClient* LiveLinkClient = GetLiveLinkClient())
			{
				LiveLinkClient->RemoveSource(StaticCastSharedPtr<ILiveLinkSource>(LiveLinkSource));
			}
		}
	}
}


void FLSALiveLinkStreamingHelper::RemoveAllSubjects()
{
	if (!IsEngineExitRequested())
	{
		if (ILiveLinkClient * LiveLinkClient = GetLiveLinkClient())
		{
			for (auto It = TrackedSubjects.CreateIterator(); It; ++It)
			{
				// Don't send packets at this point, because we're shutting the subsystem down and any
				// channels should have been closed already.
				const FLiveLinkTrackedSubject& TrackedSubject = It.Value();
				LiveLinkClient->UnregisterSubjectFramesHandle(TrackedSubject.LiveLinkSubject, TrackedSubject.StaticDataReceivedHandle, TrackedSubject.FrameDataReceivedHandle);
			}
		}

		TrackedSubjects.Empty();
	}
}

void FLSALiveLinkStreamingHelper::GetJoinInProgressPackets(TArray<TArray<uint8>>& JoinInProgressPackets)
{
	JoinInProgressPackets.Reserve(JoinInProgressPackets.Num() + TrackedSubjects.Num());

	for (auto It = TrackedSubjects.CreateIterator(); It; ++It)
	{
		// We send these packets separately, in case the connection already had the subject registered
		// but the skeleton changed since they were connected.
		FLiveLinkTrackedSubject& TrackedSubject = It.Value();
		TUniquePtr<FLSALiveLinkPacket> AddOrUpdateSubjectPacket = CreateAddOrUpdateSubjectPacket(TrackedSubject);
		if (AddOrUpdateSubjectPacket.IsValid())
		{
			TArray<uint8> PacketData;
			FMemoryWriter PacketWriter(PacketData);
			FLSALiveLinkPacket::WriteToStream(PacketWriter, *AddOrUpdateSubjectPacket);
			if (PacketData.Num() > 0)
			{
				JoinInProgressPackets.Emplace(MoveTemp(PacketData));
			}
		}
	}
}

void FLSALiveLinkStreamingHelper::ReceivedStaticData(
	FLiveLinkSubjectKey InSubjectKey,
	TSubclassOf<ULiveLinkRole> InSubjectRole,
	const FLiveLinkStaticDataStruct& InStaticData,
	const FLiveStreamAnimationHandle SubjectHandle)
{
	if (FLiveLinkTrackedSubject* TrackedSubject = TrackedSubjects.Find(SubjectHandle))
	{
		bool bSentPacket = false;
		if (const FLiveLinkSkeletonStaticData* StaticData = InStaticData.Cast<const FLiveLinkSkeletonStaticData>())
		{
			if (TrackedSubject->ReceivedStaticData(*StaticData))
			{
				bSentPacket = SendPacketToServer(CreateAddOrUpdateSubjectPacket(*TrackedSubject));
			}
			else
			{
				UE_LOG(LogLSALiveLink, Warning, TEXT("FLSALiveLinkStreamingHelper::ReceivedStaticData: Tracked Subject could not update Static Data.  Subject = (%s)"),
					*TrackedSubject->ToString());
			}
		}

		if (!bSentPacket)
		{
			UE_LOG(LogLSALiveLink, Warning, TEXT("FLSALiveLinkStreamingHelper::ReceivedStaticData: Failed to send static data packet to server. Subject = (%s)"),
				*TrackedSubject->ToString());
		}
	}
	else
	{
		UE_LOG(LogLSALiveLink, Warning, TEXT("FLSALiveLinkStreamingHelper::ReceivedStaticData: Failed to find registered subject. SubjectHandle = (%s)"),
			*SubjectHandle.ToString());
	}
}

void FLSALiveLinkStreamingHelper::ReceivedFrameData(
	FLiveLinkSubjectKey InSubjectKey,
	TSubclassOf<ULiveLinkRole> InSubjectRole,
	const FLiveLinkFrameDataStruct& InFrameData,
	const FLiveStreamAnimationHandle SubjectHandle)
{
	if (const FLiveLinkTrackedSubject* TrackedSubject = TrackedSubjects.Find(SubjectHandle))
	{
		bool bSentPacket = false;
		if (const FLiveLinkAnimationFrameData* AnimationData = InFrameData.Cast<FLiveLinkAnimationFrameData>())
		{
			FLiveLinkAnimationFrameData UpdatedFrameData;
			if (TrackedSubject->ReceivedFrameData(*AnimationData, UpdatedFrameData))
			{
				bSentPacket = SendPacketToServer(CreateAnimationFramePacket(*TrackedSubject, MoveTemp(UpdatedFrameData)));
			}
		}

		if (!bSentPacket)
		{
			UE_LOG(LogLSALiveLink, Warning, TEXT("FLSALiveLinkStreamingHelper::ReceivedFrameData: Failed to send anim packet to server. Subject = (%s)"),
				*TrackedSubject->ToString());
		}
	}
	else
	{
		UE_LOG(LogLSALiveLink, Warning, TEXT("FLSALiveLinkStreamingHelper::ReceivedFrameData: Failed to find registered subject. SubjectHandle = (%s)"),
			*SubjectHandle.ToString());
	}
}

bool FLSALiveLinkStreamingHelper::SendPacketToServer(TUniquePtr<FLSALiveLinkPacket>&& Packet)
{
	if (Packet.IsValid())
	{
		TArray<uint8> PacketData;
		FMemoryWriter Writer(PacketData);
		FLSALiveLinkPacket::WriteToStream(Writer, *Packet);

		if (PacketData.Num() > 0)
		{
			return DataHandler.SendPacketToServer(MoveTemp(PacketData), Packet->IsReliable());
		}
	}

	return false;
}

TUniquePtr<FLSALiveLinkPacket> FLSALiveLinkStreamingHelper::CreateAddOrUpdateSubjectPacket(const FLiveLinkTrackedSubject& Subject)
{
	return FLSALiveLinkAddOrUpdateSubjectPacket::CreatePacket(
			Subject.SubjectHandle,
			FLiveLinkSkeletonStaticData(Subject.LastKnownSkeleton));
}

TUniquePtr<FLSALiveLinkPacket> FLSALiveLinkStreamingHelper::CreateRemoveSubjectPacket(const FLiveLinkTrackedSubject& Subject)
{
	return FLSALiveLinkRemoveSubjectPacket::CreatePacket(Subject.SubjectHandle);
}

TUniquePtr<FLSALiveLinkPacket> FLSALiveLinkStreamingHelper::CreateAnimationFramePacket(const FLiveLinkTrackedSubject& Subject, FLiveLinkAnimationFrameData&& AnimationData)
{
	return FLSALiveLinkAnimationFramePacket::CreatePacket(
			Subject.SubjectHandle,
			FLSALiveLinkFrameData(MoveTemp(AnimationData), Subject.Options, Subject.TranslationHandle));
}

void FLSALiveLinkStreamingHelper::OnFrameTranslatorChanged()
{
	if (FLSALiveLinkSource* LocalSource = LiveLinkSource.Get())
	{
		LocalSource->SetFrameTranslator(ULSALiveLinkSettings::GetFrameTranslator());
	}
}

TSharedPtr<const FLSALiveLinkSkelMeshSource> FLSALiveLinkStreamingHelper::GetOrCreateLiveLinkSkelMeshSource()
{
	if (ELiveStreamAnimationRole::Tracker == DataHandler.GetRole())
	{
		if (!SkelMeshToLiveLinkSource.IsValid())
		{
			if (ILiveLinkClient* LiveLinkClient = GetLiveLinkClient())
			{
				SkelMeshToLiveLinkSource = MakeShared<FLSALiveLinkSkelMeshSource>();
				LiveLinkClient->AddSource(SkelMeshToLiveLinkSource);
			}
		}

		return SkelMeshToLiveLinkSource;
	}

	return nullptr;
}

ILiveLinkClient* FLSALiveLinkStreamingHelper::GetLiveLinkClient()
{
	IModularFeatures& ModularFeatures = IModularFeatures::Get();
	if (!ModularFeatures.IsModularFeatureAvailable(ILiveLinkClient::ModularFeatureName))
	{
		UE_LOG(LogLSALiveLink, Error, TEXT("GetLiveLinkClient: Live Link Unavailable."));
		return nullptr;
	}

	return &ModularFeatures.GetModularFeature<ILiveLinkClient>(ILiveLinkClient::ModularFeatureName);
}