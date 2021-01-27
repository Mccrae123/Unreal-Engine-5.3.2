// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "MetasoundBuilderInterface.h"
#include "MetasoundDataReferenceCollection.h"
#include "MetasoundExecutableOperator.h"
#include "MetasoundFacade.h"
#include "MetasoundNode.h"
#include "MetasoundNodeInterface.h"
#include "MetasoundOperatorInterface.h"
#include "MetasoundTime.h"
#include "MetasoundTrigger.h"


namespace Metasound
{
	class METASOUNDSTANDARDNODES_API FTriggerDelayNode : public FNodeFacade
	{
		public:
			FTriggerDelayNode(const FString& InName, float InDefaultDelayInSeconds);
			FTriggerDelayNode(const FNodeInitData& InInitData);

			float GetDefaultDelayInSeconds() const;

		private:
			float DefaultDelay = 1.0f;
	};
}
