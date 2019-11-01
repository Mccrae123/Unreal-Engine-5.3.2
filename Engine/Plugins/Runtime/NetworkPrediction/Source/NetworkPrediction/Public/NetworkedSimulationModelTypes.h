// Copyright 1998-2019 Epic Games, Inc. All Rights Reserved.

#pragma once

#include "NetworkedSimulationModelBuffer.h"

// ---------------------------------------------------------------------------------------------------------------------
//	BufferTypes: template helpers for addressing the different buffer types of the system.
// ---------------------------------------------------------------------------------------------------------------------

// Enum to refer to buffer type. These are used as template arguments to write generic code that can act on any of the buffers.
enum class ENetworkSimBufferTypeId : uint8
{
	Input,
	Sync,
	Aux,
	Debug
};

inline FString LexToString(ENetworkSimBufferTypeId A)
{
	switch(A)
	{
		case ENetworkSimBufferTypeId::Input: return TEXT("Input");
		case ENetworkSimBufferTypeId::Sync: return TEXT("Sync");
		case ENetworkSimBufferTypeId::Aux: return TEXT("Aux");
		case ENetworkSimBufferTypeId::Debug: return TEXT("Debug");
	};
	return TEXT("Unknown");
}

// Helper needed to specialize TNetworkSimBufferTypes::Get (must be done outside of templated struct)
template<typename TBufferTypes, ENetworkSimBufferTypeId BufferId> 
struct TSelectTypeHelper
{
	using type = void;
};

template<typename TBufferTypes>
struct TSelectTypeHelper<TBufferTypes, ENetworkSimBufferTypeId::Input>
{
	using type = typename TBufferTypes::TInputCmd;
};

template<typename TBufferTypes>
struct TSelectTypeHelper<TBufferTypes, ENetworkSimBufferTypeId::Sync>
{
	using type = typename TBufferTypes::TSyncState;
};

template<typename TBufferTypes>
struct TSelectTypeHelper<TBufferTypes, ENetworkSimBufferTypeId::Aux>
{
	using type = typename TBufferTypes::TAuxState;
};

template<typename TBufferTypes>
struct TSelectTypeHelper<TBufferTypes, ENetworkSimBufferTypeId::Debug>
{
	using type = typename TBufferTypes::TDebugState;
};

enum ENetworkSimBufferAllocationType
{
	Contiguous,
	Sparse
};

template<ENetworkSimBufferAllocationType InAllocationType, int32 InSize>
struct TNetworkSimBufferAllocation
{
	enum { Size = InSize };
	static constexpr ENetworkSimBufferAllocationType AllocationType() { return InAllocationType; }
};

template<typename T>
struct HasNetSerialize
{
	template<typename U, void (U::*)(const FNetSerializeParams& P)> struct SFINAE {};
	template<typename U> static char Test(SFINAE<U, &U::NetSerialize>*);
	template<typename U> static int Test(...);
	static const bool Has = sizeof(Test<T>(0)) == sizeof(char);
};

template<typename T>
struct HasLog
{
	template<typename U, void (U::*)(FStandardLoggingParameters& P) const> struct SFINAE {};
	template<typename U> static char Test(SFINAE<U, &U::Log>*);
	template<typename U> static int Test(...);
	static const bool Has = sizeof(Test<T>(0)) == sizeof(char);
};

// A collection of the system's buffer types. This allows us to collapse the 4 types into a single type to use a template argument elsewhere.
template<typename InInputCmd, typename InSyncState, typename InAuxState, typename InDebugState = FNetSimProcessedFrameDebugInfo>
struct TNetworkSimBufferTypes
{
	// Quick access to types when you know what you want
	using TInputCmd = InInputCmd;
	using TSyncState = InSyncState;
	using TAuxState = InAuxState;
	using TDebugState = InDebugState;

	// Template access via ENetworkSimBufferTypeId when "which buffer" is parameterized
	template<ENetworkSimBufferTypeId Id>
	struct select_type
	{
		using type = typename TSelectTypeHelper< TNetworkSimBufferTypes<TInputCmd, TSyncState, TAuxState, TDebugState>, Id >::type;
	};

	// Must implement NetSerialize and Log functions. This purposefully does not pass on inherited methods
	static_assert(HasNetSerialize<InInputCmd>::Has == true, "InputCmd Must implement NetSerialize");
	static_assert(HasNetSerialize<InSyncState>::Has == true, "SyncState Must implement NetSerialize");
	static_assert(HasNetSerialize<InAuxState>::Has == true, "AuxState Must implement NetSerialize");
	static_assert(HasNetSerialize<InDebugState>::Has == true, "DebugState Must implement NetSerialize");

	static_assert(HasLog<InInputCmd>::Has == true, "InputCmd Must implement Log");
	static_assert(HasLog<InSyncState>::Has == true, "SyncState Must implement Log");
	static_assert(HasLog<InAuxState>::Has == true, "AuxState Must implement Log");
	static_assert(HasLog<InDebugState>::Has == true, "DebugState Must implement Log");
};

// ---------------------------------------------------------------------------------------------------------------------
//	TNetworkSimBufferContainer
//	Container for the actual replicated buffers that the system uses.
//	Has compile time accessors for retrieving the buffers based on templated enum value.
// ---------------------------------------------------------------------------------------------------------------------

// Helper struct for enum-based access. This has to be done in an outside struct because we cant specialize inside TNetworkSimBufferContainer on all compilers
template<typename TContainer, ENetworkSimBufferTypeId BufferId>
struct TBufferGetterHelper
{
	static typename TContainer::template select_buffer_type<BufferId>::type& Get(TContainer& Container)
	{
		static_assert(!Container, "Failed to find specialized Get for your BufferId");
	}
};

template<typename TContainer>
struct TBufferGetterHelper<TContainer, ENetworkSimBufferTypeId::Input>
{
	static typename TContainer::TInputBuffer& Get(TContainer& Container)
	{
		return Container.Input;
	}
};
template<typename TContainer>
struct TBufferGetterHelper<TContainer, ENetworkSimBufferTypeId::Sync>
{
	static typename TContainer::TSyncBuffer& Get(TContainer& Container)
	{
		return Container.Sync;
	}
};
template<typename TContainer>
struct TBufferGetterHelper<TContainer, ENetworkSimBufferTypeId::Aux>
{
	static typename TContainer::TAuxBuffer& Get(TContainer& Container)
	{
		return Container.Aux;
	}
};
template<typename TContainer>
struct TBufferGetterHelper<TContainer, ENetworkSimBufferTypeId::Debug>
{
	static typename TContainer::TDebugBuffer& Get(TContainer& Container)
	{
		return Container.Debug;
	}
};

// -------------------------------------------------

// Helper struct for accessing a netsim buffer given the underlying type
template<typename TContainer, typename TState>
struct TBufferGetterHelper_ByState
{
	static void GetBuffer(TContainer& Container)
	{
		static_assert(!Container, "Failed to find specialized Get for your BufferId");
	}
};

template<typename TContainer>
struct TBufferGetterHelper_ByState<TContainer, typename TContainer::TInputCmd>
{
	static typename TContainer::TInputBuffer& GetBuffer(TContainer& Container)
	{
		return Container.Input;
	}
};

template<typename TContainer>
struct TBufferGetterHelper_ByState<TContainer, typename TContainer::TSyncState>
{
	static typename TContainer::TSyncBuffer& GetBuffer(TContainer& Container)
	{
		return Container.Sync;
	}
};

template<typename TContainer>
struct TBufferGetterHelper_ByState<TContainer, typename TContainer::TAuxState>
{
	static typename TContainer::TAuxBuffer& GetBuffer(TContainer& Container)
	{
		return Container.Aux;
	}
};

template<typename TContainer>
struct TBufferGetterHelper_ByState<TContainer, typename TContainer::TDebugState>
{
	static typename TContainer::TDebugBuffer& GetBuffer(TContainer& Container)
	{
		return Container.Debug;
	}
};

// -------------------------------------------------------------------

// Struct that encapsulates writing a new element to a buffer. This is used to allow a new Aux state to be created in the ::SimulationTick loop.
template<typename T>
struct TLazyStateAccessor
{
	TLazyStateAccessor(TUniqueFunction<T*()> && InFunc)
		: GetWriteNextFunc(MoveTemp(InFunc)) { }

	T* GetWriteNext() const
	{
		if (CachedWriteNext == nullptr)
		{
			CachedWriteNext = GetWriteNextFunc();
		}
		return CachedWriteNext;
	}

private:

	mutable T* CachedWriteNext = nullptr;
	TUniqueFunction<T*()> GetWriteNextFunc;
};

// The main container for all of our buffers
template<typename InBufferTypes>
struct TNetworkSimBufferContainer
{
	// Collection of types we were assigned
	using TBufferTypes = InBufferTypes;

	using TInputCmd = typename TBufferTypes::TInputCmd;
	using TSyncState = typename TBufferTypes::TSyncState;
	using TAuxState = typename TBufferTypes::TAuxState;
	using TDebugState = typename TBufferTypes::TDebugState;

	// helper that returns the buffer type for a given BufferId (not the underlying type: the actual TReplicatedBuffer<TUnderlyingType>
	template<ENetworkSimBufferTypeId TypeId>
	struct select_buffer_type
	{
		// We are just wrapping a TNetworkSimContiguousBuffer around whatever type TBufferTypes::select_type returns (which returns the underlying type)
		using type = TNetworkSimContiguousBuffer<typename TBufferTypes::template select_type<TypeId>::type>;
	};

	template<ENetworkSimBufferTypeId TypeId>
	struct select_buffer_type_sparse
	{
		// We are just wrapping a TNetworkSimSparseBuffer around whatever type TBufferTypes::select_type returns (which returns the underlying type)
		using type = TNetworkSimSparseBuffer<typename TBufferTypes::template select_type<TypeId>::type>;
	};

	// The buffer types. This may look intimidating but is not that bad!
	// We are using select_buffer_type to decide what the type should be for a given ENetworkSimBufferTypeId.
	using TInputBuffer = typename select_buffer_type<ENetworkSimBufferTypeId::Input>::type;
	using TSyncBuffer = typename select_buffer_type<ENetworkSimBufferTypeId::Sync>::type;
	using TAuxBuffer = typename select_buffer_type_sparse<ENetworkSimBufferTypeId::Aux>::type;
	using TDebugBuffer = typename select_buffer_type<ENetworkSimBufferTypeId::Debug>::type;

	// The buffers themselves. Types are already declared above.
	// If you are reading just to find the damn underlying type here, its (TNetworkSimSparseBuffer||TNetworkSimContiguousBuffer) < whatever your struct type is >
	TInputBuffer Input;
	TSyncBuffer Sync;
	TAuxBuffer Aux;
	TDebugBuffer Debug;	

	// Finally, template accessor for getting buffers based on enum. This is really what all this junk is about.
	// This allows other templated classes in the system to access a specific buffer from another templated argument
	template<ENetworkSimBufferTypeId BufferId>
	typename select_buffer_type<BufferId>::type& Get()
	{
		return TBufferGetterHelper<TNetworkSimBufferContainer<TBufferTypes>, BufferId>::Get(*this);
	}
};

// ----------------------------------------------------------------------------------------------------------------------------------------------
//		Tick and time keeping related structures
// ----------------------------------------------------------------------------------------------------------------------------------------------

// The main Simulation time type. All sims use this to talk about time.
struct FNetworkSimTime
{
	using FSimTime = int32; // Underlying type used to store simulation time
	using FRealTime = float; // Underlying type used when dealing with realtime (usually coming in from the engine tick).

	enum
	{
		RealToSimFactor = 1000 // Factor to go from RealTime (always seconds) to SimTime (MSec by default with factor of 1000)
	};

	static constexpr FRealTime GetRealToSimFactor() { return static_cast<FRealTime>(RealToSimFactor); }
	static constexpr FRealTime GetSimToRealFactor() { return static_cast<FRealTime>(1.f / RealToSimFactor); }

	// ---------------------------------------------------------------

	FNetworkSimTime() { }

	// Things get confusing with templated types and overloaded functions. To avoid that, use these funcs to construct from either msec or real time
	static inline FNetworkSimTime FromMSec(const FSimTime& InTime) { return FNetworkSimTime(InTime); } 
	static inline FNetworkSimTime FromRealTimeSeconds(const FRealTime& InRealTime) { return FNetworkSimTime( static_cast<FSimTime>(InRealTime * GetRealToSimFactor())); } 
	FRealTime ToRealTimeSeconds() const { return (Time * GetSimToRealFactor()); }

	// Direct casts to "real time MS" which should be rarely used in practice (TRealTimeAccumulator only current case). All other cases of "real time" imply seconds.
	static inline FNetworkSimTime FromRealTimeMS(const FRealTime& InRealTime) { return FNetworkSimTime( static_cast<FSimTime>(InRealTime)); } 
	FRealTime ToRealTimeMS() const { return static_cast<FRealTime>(Time); }

	FString ToString() const { return LexToString(this->Time); }

	bool IsPositive() const { return (Time > 0); }
	bool IsNegative() const { return (Time < 0); }
	void Reset() { Time = 0; }

	// FIXME
	void NetSerialize(FArchive& Ar) { Ar << Time; }

	operator FSimTime() const { return Time; }

	using T = FNetworkSimTime;
	T& operator+= (const T &rhs) { this->Time += rhs.Time; return(*this); }
	T& operator-= (const T &rhs) { this->Time -= rhs.Time; return(*this); }
	
	T operator+ (const T &rhs) const { return T(this->Time + rhs.Time); }
	T operator- (const T &rhs) const { return T(this->Time - rhs.Time); }

	bool operator<  (const T &rhs) const { return(this->Time < rhs.Time); }
	bool operator<= (const T &rhs) const { return(this->Time <= rhs.Time); }
	bool operator>  (const T &rhs) const { return(this->Time > rhs.Time); }
	bool operator>= (const T &rhs) const { return(this->Time >= rhs.Time); }
	bool operator== (const T &rhs) const { return(this->Time == rhs.Time); }
	bool operator!= (const T &rhs) const { return(this->Time != rhs.Time); }

private:

	FSimTime Time = 0;

	FNetworkSimTime(const FSimTime& InTime) { Time = InTime; }
};

// Holds per-simulation settings about how ticking is supposed to happen.
template<int32 InFixedStepMS=0, int32 InMaxStepMS=0>
struct TNetworkSimTickSettings
{
	static_assert(!(InFixedStepMS!=0 && InMaxStepMS != 0), "MaxStepMS is only applicable when using variable step (FixedStepMS == 0)");
	enum 
	{
		MaxStepMS = InMaxStepMS,				// Max step. Only applicable to variable time step.
		FixedStepMS = InFixedStepMS,			// Fixed step. If 0, then we are "variable time step"
	};

	// Typed accessors
	static constexpr FNetworkSimTime::FSimTime GetMaxStepMS() { return static_cast<FNetworkSimTime::FSimTime>(InMaxStepMS); }
	static constexpr FNetworkSimTime::FSimTime GetFixedStepMS() { return static_cast<FNetworkSimTime::FSimTime>(InFixedStepMS); }
};

// ----------------------------------------------------------------------------------------------------------------------------------------------
//	Accumulator: Helper for accumulating real time into sim time based on TickSettings
// ----------------------------------------------------------------------------------------------------------------------------------------------

template<typename TickSettings, bool IsFixedTick=(TickSettings::FixedStepMS!=0)>
struct TRealTimeAccumulator
{
	using TRealTime = FNetworkSimTime::FRealTime;
	void Accumulate(FNetworkSimTime& NetworkSimTime, const TRealTime RealTimeSeconds)
	{
		// Even though we are variable tick, we still want to truncate down to an even msec. This keeps sim steps as whole integer values that serialize better, don't have denormals or other floating point weirdness.
		// (If we ever wanted to just fully let floats pass through and be used as the msec sim time, this could be done through another specialization for float/float time).		
		// Also note that MaxStepMS enforcement does NOT belong here. Dropping time due to MaxStepMS would just make the sim run slower. MaxStepMS is used at the input processing level.
		
		AccumulatedTimeMS += RealTimeSeconds * FNetworkSimTime::GetRealToSimFactor(); // convert input seconds -> msec
		const FNetworkSimTime AccumulatedSimTimeMS = FNetworkSimTime::FromRealTimeMS(AccumulatedTimeMS);	// truncate (float) accumulated msec to (int32) sim time msec

		NetworkSimTime += AccumulatedSimTimeMS;
		AccumulatedTimeMS -= AccumulatedSimTimeMS.ToRealTimeMS(); // subtract out the "whole" msec, we are left with the remainder msec
	}

	void Reset()
	{
		AccumulatedTimeMS = 0.f;
	}

private:

	TRealTime AccumulatedTimeMS = 0.f;
};

// Specialized version of FixedTicking. This accumulates real time that spills over into NetworkSimTime as it crosses the FixStep threshold
template<typename TickSettings>
struct TRealTimeAccumulator<TickSettings, true>
{
	using TRealTime = FNetworkSimTime::FRealTime;
	const TRealTime RealTimeFixedStep = static_cast<TRealTime>(TickSettings::GetFixedStepMS() * FNetworkSimTime::GetSimToRealFactor());

	void Accumulate(FNetworkSimTime& NetworkSimTime, const TRealTime RealTimeSeconds)
	{
		AccumulatedTime += RealTimeSeconds;
		if (AccumulatedTime > RealTimeFixedStep)
		{
			const int32 NumFrames = AccumulatedTime / RealTimeFixedStep;
			AccumulatedTime -= NumFrames * RealTimeFixedStep;
				
			if (FMath::Abs<TRealTime>(AccumulatedTime) < SMALL_NUMBER)
			{
				AccumulatedTime = TRealTime(0.f);
			}

			NetworkSimTime += FNetworkSimTime::FromMSec(NumFrames * TickSettings::FixedStepMS);
		}
	}

	void Reset()
	{
		AccumulatedTime = 0.f;
	}

private:

	TRealTime AccumulatedTime;
};

// ----------------------------------------------------------------------------------------------------------------------------------------------
//	FSimulationTickState: Holds active state for simulation ticking. We track two things: frames and time.
//
//	PendingFrame is the next frame we will process: the input/sync/aux state @ PendingFrame will be run through ::SimulationTick and produce the 
//	next frame's (PendingFrame+1) Sync and possibly Aux state (if it changes). "Out of band" modifications to the sync/aux state should happen
//	to PendingFrame (e.g, before it is processed. Once a frame has been processed, we won't run it through ::SimulationTick again!).
//
//	MaxAllowedFrame is a frame based limiter on simulation updates. This must be incremented to allow the simulation to advance.
//
//	Time is also tracked. We keep running total for how much the sim has advanced and how much it is allowed to advance. There is also a historic buffer of
//	simulation time in SimulationTimeBuffer.
//
//	Consider that Frames are essentially client dependent and gaps can happen due to packet loss, etc. Time will always be continuous though.
//	
// ----------------------------------------------------------------------------------------------------------------------------------------------

struct FSimulationTickState
{
	int32 PendingFrame = 0;
	int32 MaxAllowedFrame = -1;
	bool bUpdateInProgress = false;
	
	FNetworkSimTime GetTotalProcessedSimulationTime() const 
	{ 
		return TotalProcessedSimulationTime; 
	}

	void SetTotalProcessedSimulationTime(const FNetworkSimTime& SimTime, int32 Frame)
	{
		TotalProcessedSimulationTime = SimTime;
		*SimulationTimeBuffer.WriteFrame(Frame) = SimTime;
	}

	void IncrementTotalProcessedSimulationTime(const FNetworkSimTime& DeltaSimTime, int32 Frame)
	{
		TotalProcessedSimulationTime += DeltaSimTime;
		*SimulationTimeBuffer.WriteFrame(Frame) = TotalProcessedSimulationTime;
	}
		
	// Historic tracking of simulation time. This allows us to timestamp sync data as its produced
	TNetworkSimContiguousBuffer<FNetworkSimTime>	SimulationTimeBuffer;

	// How much granted simulation time is left to process
	FNetworkSimTime GetRemainingAllowedSimulationTime() const
	{
		return TotalAllowedSimulationTime - TotalProcessedSimulationTime;
	}

	FNetworkSimTime GetTotalAllowedSimulationTime() const
	{
		return TotalAllowedSimulationTime;
	}

protected:

	FNetworkSimTime TotalAllowedSimulationTime;	// Total time we have been "given" to process. We cannot process more simulation time than this: doing so would be speed hacking.
	FNetworkSimTime TotalProcessedSimulationTime;	// How much time we've actually processed. The only way to increment this is to process user commands or receive authoritative state from the network.
};

// "Ticker" that actually allows us to give the simulation time. This struct will generally not be passed around outside of the core TNetworkedSimulationModel/Replicators
template<typename TickSettings=TNetworkSimTickSettings<>>
struct TSimulationTicker : public FSimulationTickState
{
	using TSettings = TickSettings;

	void SetTotalAllowedSimulationTime(const FNetworkSimTime& SimTime)
	{
		TotalAllowedSimulationTime = SimTime;
		RealtimeAccumulator.Reset();
	}

	// "Grants" allowed simulation time to this tick state. That is, we are now allowed to advance the simulation by this amount the next time the sim ticks.
	// Note the input is RealTime in SECONDS. This is what the rest of the engine uses when dealing with float delta time.
	void GiveSimulationTime(float RealTimeSeconds)
	{
		RealtimeAccumulator.Accumulate(TotalAllowedSimulationTime, RealTimeSeconds);
	}	

private:

	TRealTimeAccumulator<TSettings>	RealtimeAccumulator;	
};

// Scoped helper to be used right before entering a call to the sim's ::SimulationTick function.
// Important to note that this advances the PendingFrame to the output Frame. So that any writes that occur to the buffers during this scope will go to the output frame.
struct TScopedSimulationTick
{
	TScopedSimulationTick(FSimulationTickState& InTicker, const int32& InOutputFrame, const FNetworkSimTime& InDeltaSimTime)
		: Ticker(InTicker), OutputFrame(InOutputFrame), DeltaSimTime(InDeltaSimTime)
	{
		check(Ticker.bUpdateInProgress == false);
		Ticker.PendingFrame = OutputFrame;
		Ticker.bUpdateInProgress = true;
	}
	~TScopedSimulationTick()
	{
		Ticker.IncrementTotalProcessedSimulationTime(DeltaSimTime, OutputFrame);
		Ticker.bUpdateInProgress = false;
	}
	FSimulationTickState& Ticker;
	const int32& OutputFrame;
	const FNetworkSimTime& DeltaSimTime;
};

// ----------------------------------------------------------------------------------------------------------------------------------------------
//	Accessors - helper structs that provide safe/cleaner access to the underlying NetSim states/events
// ----------------------------------------------------------------------------------------------------------------------------------------------

// Accessor conditionally gives access to the current (pending) Sync/Aux state to outside code.
// Reads are always allowed
// Writes are conditional. Authority can always write to the pending frame. Non authority requires the netsim to be currently processing an ::SimulationTick.
// If you aren't inside an ::SimulationTick call, it is really not safe to predict state changes. It is safest and simplest to just not predict these changes.
//
// Explanation: During the scope of an ::SimulationTick call, we know exactly 'when' we are relative to what the server is processing. If the predicting client wants
// to predict a change to sync/aux state during an update, the server will do it at the exact same time (assuming not a mis prediction). When a state change
// happens "out of band" (outside an ::SimulationTick call) - we really have no way to correlate when the server will do it. While its tempting to think "we will get
// a correction anyways, might as well guess at it and maybe get a smaller correction" - but this opens us up to other problems. The server may actually not 
// change the state at all and you may not get an update that corrects you. You could add a timeout and track the state change somewhere but that really complicates
// things and could leave you open to "double" problems: if the state change is additive, you may stack the authority change on top of the local predicted change, or
// you may roll back the predicted change to then later receive the authority change.
//	
// What still may make sense to do is allow the "In Update" bool to be temporarily disabled if we enter code that we know is not rollback friendly.
template<typename TState>
struct TNetworkSimStateAccessor
{
	template<typename TNetworkSimModel>
	void Init(TNetworkSimModel* NetSimModel)
	{
		GetStateFunc = [NetSimModel](bool bWrite, TState*& OutState, bool& OutSafe)
		{
			// Gross: find the buffer our TState is in. This allows these accessors to be declared as class member variables without knowing the exact net sim it will
			// be pulling from. (For example, you may want to templatize your network sim model).
			auto& Buffer = TBufferGetterHelper_ByState<TNetworkSimBufferContainer<typename TNetworkSimModel::TBufferTypes>, TState>::GetBuffer(NetSimModel->Buffers);
			OutState = bWrite ? Buffer.WriteFrameInitializedFromHead(NetSimModel->Ticker.PendingFrame) : Buffer[NetSimModel->Ticker.PendingFrame];
			OutSafe = NetSimModel->Ticker.bUpdateInProgress;
		};
	}

	void Clear()
	{
		GetStateFunc = nullptr;
	}

	/** Gets the current (PendingFrame) state for reading. This is not expected to fail outside of startup/shutdown edge cases */
	const TState* GetStateRead() const
	{
		TState* State = nullptr;
		bool bSafe = false;
		if (GetStateFunc)
		{
			GetStateFunc(false, State, bSafe);
		}
		return State;
	}

	/** Gets the current (PendingFrame) state for writing. This is expected to fail outside of the core update loop when bHasAuthority=false. (E.g, it is not safe to predict writes) */
	TState* GetStateWrite(bool bHasAuthority) const
	{
		TState* State = nullptr;
		bool bSafe = false;
		if (GetStateFunc)
		{
			GetStateFunc(true, State, bSafe);
		}
		return (bHasAuthority || bSafe) ? State : nullptr;
	}

private:

	TFunction<void(bool bForWrite, TState*& OutState, bool& OutSafe)> GetStateFunc;
};

//TODO
struct FNetworkSimEventAccessor
{
	int32 GetPendingFrame() { return -1; }

	struct FFrameEvents
	{
		/** This frame has been received from the authority and will not be rolled back or resimulated again. */
		DECLARE_MULTICAST_DELEGATE(FOnFrameConfirmed)
		FOnFrameConfirmed Confirmed;

		/** This frame was previously simulated and the simulation has now been rolled back */
		DECLARE_MULTICAST_DELEGATE(FOnFrameRolledBack)
		FOnFrameRolledBack RolledBack;

		DECLARE_MULTICAST_DELEGATE(FOnFrameSimulated)
		FOnFrameRolledBack Simulated;
	};
};

// ----------------------------------------------------------------------------------------------------------------------------------------------
//	FrameCmd - in variable tick simulations we store the timestep of each frame with the input. TFrameCmd wraps the user struct to do this.
// ----------------------------------------------------------------------------------------------------------------------------------------------

// Wraps an input command (BaseType) in a NetworkSimulation time. 
template<typename BaseCmdType, typename TickSettings, bool IsFixedTick=(TickSettings::FixedStepMS!=0)>
struct TFrameCmd : public BaseCmdType
{
	FNetworkSimTime GetFrameDeltaTime() const { return FrameDeltaTime; }
	void SetFrameDeltaTime(const FNetworkSimTime& InTime) { FrameDeltaTime = InTime; }
	void NetSerialize(const FNetSerializeParams& P)
	{
		FrameDeltaTime.NetSerialize(P.Ar);
		BaseCmdType::NetSerialize(P); 
	}

	void Log(FStandardLoggingParameters& P) const { BaseCmdType::Log(P); }

private:
	FNetworkSimTime FrameDeltaTime;
};

// Fixed tick specialization
template<typename BaseCmdType, typename TickSettings>
struct TFrameCmd<BaseCmdType, TickSettings, true> : public BaseCmdType
{
	FNetworkSimTime GetFrameDeltaTime() const { return FNetworkSimTime::FromMSec(TickSettings::GetFixedStepMS()); }
	void SetFrameDeltaTime(const FNetworkSimTime& InTime) { }
	void NetSerialize(const FNetSerializeParams& P) { BaseCmdType::NetSerialize(P); }
	void Log(FStandardLoggingParameters& P) const { BaseCmdType::Log(P); }
};

// Helper to turn user supplied buffer types into the "real" buffer types: the InputCmd struct is wrapped in TFrameCmd
template<typename TUserBufferTypes, typename TTickSettings>
struct TInternalBufferTypes : TNetworkSimBufferTypes< 
	
	// InputCmds are wrapped in TFrameCmd, which will store an explicit sim delta time if we are not a fixed tick sim
	TFrameCmd< typename TUserBufferTypes::TInputCmd , TTickSettings>,

	typename TUserBufferTypes::TSyncState,	// SyncState passes through
	typename TUserBufferTypes::TAuxState,	// Auxstate passes through
	typename TUserBufferTypes::TDebugState	// Debugstate passes through
>
{
};

/** This is the "system driver", it has functions that the TNetworkedSimulationModel needs to call internally, that are specific to the types but not specific to the simulation itself. */
template<typename TBufferTypes>
class TNetworkedSimulationModelDriver
{
public:
	using TInputCmd = typename TBufferTypes::TInputCmd;
	using TSyncState = typename TBufferTypes::TSyncState;
	using TAuxState= typename TBufferTypes::TAuxState;

	// Debug string that can be used in internal warning/error logs
	virtual FString GetDebugName() const = 0;

	// Owning object for Visual Logs so that the system can emit them internally
	virtual const AActor* GetVLogOwner() const = 0;

	// Call to visual log the given states. Note that not all 3 will always be present and you should check for nullptrs.
	virtual void VisualLog(const TInputCmd* Input, const TSyncState* Sync, const TAuxState* Aux, const FVisualLoggingParameters& SystemParameters) const = 0;
	
	// Called whenever the sim is ready to process new local input.
	virtual void ProduceInput(const FNetworkSimTime SimTime, TInputCmd&) = 0;
	
	// Called from the Network Sim at the end of the sim frame when there is new sync data.
	virtual void FinalizeFrame(const typename TBufferTypes::TSyncState& SyncState, const typename TBufferTypes::TAuxState& AuxState) = 0;
};

// ----------------------------------------------------------------------------------------------------------------------------------------------
//	SimulationTick parameters. These are the data structures passed into the simulation code each frame
// ----------------------------------------------------------------------------------------------------------------------------------------------

struct TNetSimTimeStep
{
	// The delta time step for this tick (in MS by default)
	const FNetworkSimTime& StepMS;
	// The tick state of the simulation prior to running this tick. E.g, does not "include" the above StepMS that we are simulating now.
	// The first time ::SimulationTick runs, TickState.GetTotalProcessedSimulationTime() == 0.
	const FSimulationTickState& TickState;
};

// Input state: const references to the InputCmd/SyncState/AuxStates
template<typename TBufferTypes>
struct TNetSimInput
{
	const typename TBufferTypes::TInputCmd& Cmd;
	const typename TBufferTypes::TSyncState& Sync;
	const typename TBufferTypes::TAuxState& Aux;

	TNetSimInput(const typename TBufferTypes::TInputCmd& InInputCmd, const typename TBufferTypes::TSyncState& InSync, const typename TBufferTypes::TAuxState& InAux)
		: Cmd(InInputCmd), Sync(InSync), Aux(InAux) { }	
	
	// Allows implicit downcasting to a parent simulation class's input types
	template<typename T>
	TNetSimInput(const TNetSimInput<T>& Other)
		: Cmd(Other.Cmd), Sync(Other.Sync), Aux(Other.Aux) { }
};

// Output state: the output SyncState (always created) and TNetSimLazyWriter for the AuxState (created on demand since every tick does not generate a new aux frame)
template<typename TBufferTypes>
struct TNetSimOutput
{
	typename TBufferTypes::TSyncState& Sync;
	const TNetSimLazyWriter<typename TBufferTypes::TAuxState>& Aux;

	TNetSimOutput(typename TBufferTypes::TSyncState& InSync, const TNetSimLazyWriter<typename TBufferTypes::TAuxState>& InAux)
		: Sync(InSync), Aux(InAux) { }
};

