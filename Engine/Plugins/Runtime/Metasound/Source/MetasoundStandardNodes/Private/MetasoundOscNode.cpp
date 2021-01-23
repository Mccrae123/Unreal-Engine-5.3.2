// Copyright Epic Games, Inc. All Rights Reserved.

#include "MetasoundOscNode.h"

#include "MetasoundAudioBuffer.h"
#include "MetasoundExecutableOperator.h"
#include "MetasoundFacade.h"
#include "MetasoundFrequency.h"
#include "MetasoundNodeRegistrationMacro.h"
#include "MetasoundPrimitives.h"
#include "MetasoundVertex.h"
#include "MetasoundBop.h"
#include "DSP/Dsp.h"

#define LOCTEXT_NAMESPACE "MetasoundStandardNodes"

namespace Metasound
{
	class FOscOperator : public TExecutableOperator<FOscOperator>
	{
	public:

		static TUniquePtr<IOperator> CreateOperator(const FCreateOperatorParams& InParams, FBuildErrorArray& OutErrors);
		static const FNodeInfo& GetNodeInfo();
		static FVertexInterface DeclareVertexInterface();

		FOscOperator(const FOperatorSettings& InSettings, const FFrequencyReadRef& InFrequency, const FBoolReadRef& InActivate);

		virtual FDataReferenceCollection GetInputs() const override;

		virtual FDataReferenceCollection GetOutputs() const override;

		void Execute();

	private:
		// Returns true if there was a phase wrap this update
		FORCEINLINE bool WrapPhase(float InPhaseInc, float& OutPhase)
		{
			bool Result = false;
			if (InPhaseInc > 0.0f && OutPhase >= 1.0f)
			{
				OutPhase = FMath::Fmod(OutPhase, 1.0f);
				Result = true;
			}
			else if (InPhaseInc < 0.0f && OutPhase <= 0.0f)
			{
				OutPhase = FMath::Fmod(OutPhase, 1.0f) + 1.0f;
				Result = true;
			}
			return Result;
		}

		static constexpr const float TwoPi = 2.f * PI;

		// The current phase of oscillator (between 0.0 and 1.0)
		float Phase;
		float OneOverSampleRate;
		float Nyquist;

		FFrequencyReadRef Frequency;
		FBoolReadRef Enabled;
		FAudioBufferWriteRef AudioBuffer;

		static constexpr const TCHAR* EnabledPinName = TEXT("Enabled");
		static constexpr const TCHAR* FrequencyPinName = TEXT("Frequency");
		static constexpr const TCHAR* AudioOutPinName = TEXT("Audio");		
	};

	FOscOperator::FOscOperator(const FOperatorSettings& InSettings, const FFrequencyReadRef& InFrequency, const FBoolReadRef& InEnabled)
		: Phase(0.f)
		, OneOverSampleRate(1.f / InSettings.GetSampleRate())
		, Nyquist(InSettings.GetSampleRate() / 2.0f)
		, Frequency(InFrequency)
		, Enabled(InEnabled)
		, AudioBuffer(FAudioBufferWriteRef::CreateNew(InSettings))
	{
		check(AudioBuffer->Num() == InSettings.GetNumFramesPerBlock());
	}

	FDataReferenceCollection FOscOperator::GetInputs() const
	{
		FDataReferenceCollection InputDataReferences;
		InputDataReferences.AddDataReadReference(FrequencyPinName, FFrequencyReadRef(Frequency));
		InputDataReferences.AddDataReadReference(EnabledPinName, FBoolReadRef(Enabled));

		return InputDataReferences;
	}

	FDataReferenceCollection FOscOperator::GetOutputs() const
	{
		FDataReferenceCollection OutputDataReferences;
		OutputDataReferences.AddDataReadReference(AudioOutPinName, FAudioBufferReadRef(AudioBuffer));
		return OutputDataReferences;
	}

	void FOscOperator::Execute()
	{
		// Clamp frequencies into Nyquist range
		const float Freq = FMath::Clamp(Frequency->GetHertz(), -Nyquist, Nyquist);
		const float PhaseInc = Freq * OneOverSampleRate;
		float* Data = AudioBuffer->GetData();

		FMemory::Memzero(Data, AudioBuffer->Num() * sizeof(float));

		if (*Enabled)
		{
			for (int32 i = 0; i < AudioBuffer->Num(); i++)
			{
				// This is borrowed from the FOsc class with the intention to eventually recreate that functionality here.
				// We don't wish to use it directly as it has a virtual call for each sample. Phase is in cents with the intent
				// to support other oscillator types in time.

				const float Radians = (Phase * TwoPi) - PI;
				Data[i] = Audio::FastSin3(-1.0f * Radians);
				Phase += PhaseInc;
				WrapPhase(PhaseInc, Phase);
			}
		}
	}

	FVertexInterface FOscOperator::DeclareVertexInterface()
	{
		static const FVertexInterface Interface(
			FInputVertexInterface(
				TInputDataVertexModel<FFrequency>(FrequencyPinName, LOCTEXT("OscFrequencyDescription", "The frequency of oscillator.")),
				TInputDataVertexModel<bool>(EnabledPinName, LOCTEXT("OscActivateDescription", "Enable the oscilator."))
			),
			FOutputVertexInterface(
				TOutputDataVertexModel<FAudioBuffer>(TEXT("Audio"), LOCTEXT("AudioTooltip", "The output audio"))
			)
		);

		return Interface;
	}


	const FNodeInfo& FOscOperator::GetNodeInfo()
	{
		auto InitNodeInfo = []() -> FNodeInfo
		{
			FNodeInfo Info;
			Info.ClassName = FName(TEXT("Osc"));
			Info.MajorVersion = 1;
			Info.MinorVersion = 0;
			Info.Description = LOCTEXT("Metasound_OscNodeDescription", "Emits an audio signal of a sinusoid.");
			Info.Author = PluginAuthor;
			Info.PromptIfMissing = PluginNodeMissingPrompt;
			Info.DefaultInterface = DeclareVertexInterface();

			return Info;
		};

		static const FNodeInfo Info = InitNodeInfo();

		return Info;
	}

	FOscNode::FOscNode(const FString& InName, float InDefaultFrequency, bool bInDefaultEnablement)
	:	FNodeFacade(InName, TFacadeOperatorClass<FOscOperator>())
	,	DefaultFrequency(InDefaultFrequency)
	,	bDefaultEnablement(bInDefaultEnablement)
	{
	}

	FOscNode::FOscNode(const FNodeInitData& InInitData)
		: FOscNode(InInitData.InstanceName, 440.0f, true)
	{
	}

	float FOscNode::GetDefaultFrequency() const
	{
		return DefaultFrequency;
	}

	bool FOscNode::GetDefaultEnablement() const
	{
		return bDefaultEnablement;
	}

	TUniquePtr<IOperator> FOscOperator::CreateOperator(const FCreateOperatorParams& InParams, FBuildErrorArray& OutErrors) 
	{
		const FOscNode& OscNode = static_cast<const FOscNode&>(InParams.Node);
		const FDataReferenceCollection& InputCol = InParams.InputDataReferences;
		const FOperatorSettings& Settings = InParams.OperatorSettings;

		FFrequencyReadRef Frequency = InputCol.GetDataReadReferenceOrConstruct<FFrequency>(FrequencyPinName, OscNode.GetDefaultFrequency(), EFrequencyResolution::Hertz);
		FBoolReadRef Enabled = InputCol.GetDataReadReferenceOrConstruct<bool>(EnabledPinName, OscNode.GetDefaultEnablement());

		return MakeUnique<FOscOperator>(InParams.OperatorSettings, Frequency, Enabled);
	}

	METASOUND_REGISTER_NODE(FOscNode);
}
#undef LOCTEXT_NAMESPACE //MetasoundOscNode

