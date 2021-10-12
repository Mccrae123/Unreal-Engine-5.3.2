// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "Online/OnlineAsyncOpHandle.h"
#include "Online/OnlineTypeInfo.h"

#include "Containers/Map.h"
#include "Templates/UniquePtr.h"
#include "Async/Future.h"
#include "Async/Async.h"

namespace UE::Online {

class FOnlineServicesCommon;

// TEMP
namespace Errors
{
	inline FOnlineError Unknown() { return FOnlineError();  }
}
// END TEMP

class FOnlineAsyncOp;
template <typename OpType> class TOnlineAsyncOp;
template <typename OpType, typename T> class TOnlineChainableAsyncOp;

enum class EOnlineAsyncExecutionPolicy : uint8
{
	RunOnGameThread,	// Run on the game thread, will execute immediately if we are already on the game thread
	RunOnNextTick,		// Run on the game thread next time we tick
	RunOnThreadPool,	// Run on a specified thread pool
	RunOnTaskGraph,		// Run on the task graph
	RunImmediately		// Call immediately, in the current thread
};

// TODO
class FOnlineAsyncExecutionPolicy
{
public:
	FOnlineAsyncExecutionPolicy(EOnlineAsyncExecutionPolicy InExecutionPolicy)
		: ExecutionPolicy(InExecutionPolicy)
	{
	}

	static FOnlineAsyncExecutionPolicy RunOnGameThread() { return FOnlineAsyncExecutionPolicy(EOnlineAsyncExecutionPolicy::RunOnGameThread); }
	static FOnlineAsyncExecutionPolicy RunOnNextTick() { return FOnlineAsyncExecutionPolicy(EOnlineAsyncExecutionPolicy::RunOnNextTick); }
	static FOnlineAsyncExecutionPolicy RunOnThreadPool() { return FOnlineAsyncExecutionPolicy(EOnlineAsyncExecutionPolicy::RunOnThreadPool); } // TODO: allow thread pool to be specified
	static FOnlineAsyncExecutionPolicy RunOnTaskGraph() { return FOnlineAsyncExecutionPolicy(EOnlineAsyncExecutionPolicy::RunOnTaskGraph); }
	static FOnlineAsyncExecutionPolicy RunImmediately() { return FOnlineAsyncExecutionPolicy(EOnlineAsyncExecutionPolicy::RunImmediately); }

	const EOnlineAsyncExecutionPolicy& GetExecutionPolicy() const { return ExecutionPolicy; }

private:
	EOnlineAsyncExecutionPolicy ExecutionPolicy;
};


namespace Private
{

class FOnlineOperationData // Map of (TypeName,Key)->(data of any Type)
{
public:
	template <typename T>
	void Set(const FString& Key, T&& InData)
	{
		Data.Add(FOperationDataKey{ TOnlineTypeInfo<T>::GetTypeName(), Key }, MakeUnique<TData<T>>(MoveTemp(InData)));
	}

	template <typename T>
	void Set(const FString& Key, const T& InData)
	{
		Data.Add(FOperationDataKey{ TOnlineTypeInfo<T>::GetTypeName(), Key }, MakeUnique<TData<T>>(InData));
	}

	template <typename T>
	const T* Get(const FString& Key) const
	{
		if (auto Value = Data.Find(FOperationDataKey{ TOnlineTypeInfo<T>::GetTypeName(), Key }))
		{
			return static_cast<const T*>((*Value)->GetData());
		}

		return nullptr;
	}

	struct FOperationDataKey
	{
		FOnlineTypeName TypeName;
		FString Key;

		bool operator==(const FOperationDataKey& Other) const
		{
			return TypeName == Other.TypeName && Key == Other.Key;
		}
	};

private:
	class IData
	{
	public:
		virtual ~IData() {}
		virtual FOnlineTypeName GetTypeName() = 0;
		virtual void* GetData() = 0;

		template <typename T>
		const T* Get()
		{
			if (GetTypeName() == TOnlineTypeInfo<T>::GetTypeName())
			{
				return static_cast<T*>(GetData());
			}

			return nullptr;
		}
	};

	template <typename T>
	class TData : public IData
	{
	public:
		TData(const T& InData)
			: Data(InData)
		{
		}

		TData(T&& InData)
			: Data(MoveTemp(InData))
		{
		}

		virtual FOnlineTypeName GetTypeName() override
		{
			return  TOnlineTypeInfo<T>::GetTypeName();
		}

		virtual void* GetData() override
		{
			return &Data;
		}

	private:
		T Data;
	};

	friend uint32 GetTypeHash(const FOperationDataKey& Key);

	TMap<FOperationDataKey, TUniquePtr<IData>> Data;
};

inline uint32 GetTypeHash(const FOnlineOperationData::FOperationDataKey& Key)
{
	using ::GetTypeHash;
	return HashCombine(GetTypeHash(Key.TypeName), GetTypeHash(Key.Key));
}

template <typename... Params>
struct TOnlineAsyncOpCallableTraitsHelper2
{
};

template <typename TResultType, typename OpType>
struct TOnlineAsyncOpCallableTraitsHelper2<TResultType, OpType&>
{
	using ParamType = void;
	using ResultType = TResultType;
	static constexpr bool bAsyncResult = false;
	static constexpr bool bRequiresPromise = false;
};

template <typename TResultType, typename OpType, typename TParamType>
struct TOnlineAsyncOpCallableTraitsHelper2<TResultType, OpType&, TParamType>
{
	using ParamType = TParamType;
	using ResultType = TResultType;
	static constexpr bool bAsyncResult = false;
	static constexpr bool bRequiresPromise = false;
};

template <typename TResultType, typename OpType>
struct TOnlineAsyncOpCallableTraitsHelper2<TFuture<TResultType>, OpType&>
{
	using ParamType = void;
	using ResultType = TResultType;
	static constexpr bool bAsyncResult = true;
	static constexpr bool bRequiresPromise = false;
};

template <typename TResultType, typename OpType, typename TParamType>
struct TOnlineAsyncOpCallableTraitsHelper2<TFuture<TResultType>, OpType&, TParamType>
{
	using ParamType = TParamType;
	using ResultType = TResultType;
	static constexpr bool bAsyncResult = true;
	static constexpr bool bRequiresPromise = false;
};

template <typename TResultType, typename OpType>
struct TOnlineAsyncOpCallableTraitsHelper2<void, OpType&, TPromise<TResultType>&&>
{
	using ParamType = void;
	using ResultType = TResultType;
	static constexpr bool bAsyncResult = true;
	static constexpr bool bRequiresPromise = true;
};

template <typename TResultType, typename OpType, typename TParamType>
struct TOnlineAsyncOpCallableTraitsHelper2<void, OpType&, TParamType, TPromise<TResultType>&&>
{
	using ParamType = TParamType;
	using ResultType = TResultType;
	static constexpr bool bAsyncResult = true;
	static constexpr bool bRequiresPromise = true;
};

template <typename CallableType>
struct TOnlineAsyncOpCallableTraitsHelper
{
};

template <typename ReturnType, typename... ParamTypes>
struct TOnlineAsyncOpCallableTraitsHelper<ReturnType(ParamTypes...)>
	: public TOnlineAsyncOpCallableTraitsHelper2<ReturnType, ParamTypes...>
{
};

template <typename ReturnType, typename ObjectType, typename... ParamTypes>
struct TOnlineAsyncOpCallableTraitsHelper<ReturnType(ObjectType::*)(ParamTypes...)>
	: public TOnlineAsyncOpCallableTraitsHelper2<ReturnType, ParamTypes...>
{
};

template <typename ReturnType, typename ObjectType, typename... ParamTypes>
struct TOnlineAsyncOpCallableTraitsHelper<ReturnType(ObjectType::*)(ParamTypes...) const>
	: public TOnlineAsyncOpCallableTraitsHelper2<ReturnType, ParamTypes...>
{
};

template <typename CallableType, typename = void>
struct TOnlineAsyncOpCallableTraits
{
};

// function pointers
template <typename CallableFunction>
struct TOnlineAsyncOpCallableTraits<CallableFunction, std::enable_if_t<std::is_function_v<std::remove_pointer_t<CallableFunction>>, void>>
	: public TOnlineAsyncOpCallableTraitsHelper<CallableFunction>
{
};

// lambdas, TFunction, functor objects (anything with operator())
template <typename CallableObject>
struct TOnlineAsyncOpCallableTraits<CallableObject, std::enable_if_t<!std::is_function_v<std::remove_pointer_t<CallableObject>>, void>>
	: public TOnlineAsyncOpCallableTraitsHelper<decltype(&std::remove_reference_t<CallableObject>::operator())>
{
};


class IStep
{
public:
	virtual ~IStep() {}
	virtual const FOnlineAsyncExecutionPolicy& GetExecutionPolicy() const = 0;
	virtual void Execute() = 0;
};

template <typename ResultType>
class TStep : public IStep
{
public:
	TStep(FOnlineAsyncExecutionPolicy&& InExecutionPolicy)
		: ExecutionPolicy(MoveTemp(InExecutionPolicy))
	{
	}

	template <typename OpType, typename LastResultType, typename CallableType>
	void SetExecFunction(TOnlineAsyncOp<OpType>& InOperation, LastResultType&& InLastResult, CallableType&& InCallable)
	{
		ExecFunction = [this, WeakOperation = TWeakPtr<TOnlineAsyncOp<OpType>>(InOperation.AsShared()), &LastResult = InLastResult, Callable = MoveTemp(InCallable)]()
		{
			TSharedPtr<TOnlineAsyncOp<OpType>> PinnedOperation = WeakOperation.Pin();
			if (PinnedOperation)
			{
				if constexpr (TOnlineAsyncOpCallableTraits<CallableType>::bRequiresPromise)
				{
					TPromise<ResultType> Promise;
					// set promise continuation before calling the callable so that we will complete the step as soon as the value is set
					Promise.GetFuture()
						.Next([this, WeakOperation](const ResultType& Value)
						{
							TSharedPtr<TOnlineAsyncOp<OpType>> PinnedOperation2 = WeakOperation.Pin();
							if (PinnedOperation2)
							{
								Result = Value;
								PinnedOperation2->ExecuteNextStep();
							}
						});

					Callable(*PinnedOperation, MoveTempIfPossible(LastResult), MoveTemp(Promise));
				}
				else if constexpr (TOnlineAsyncOpCallableTraits<CallableType>::bAsyncResult)
				{
					Callable(*PinnedOperation, MoveTempIfPossible(LastResult))
						.Next([this, WeakOperation](const ResultType& Value)
						{
							TSharedPtr<TOnlineAsyncOp<OpType>> PinnedOperation2 = WeakOperation.Pin();
							if (PinnedOperation2)
							{
								Result = Value;
								PinnedOperation2->ExecuteNextStep();
							}
						});
				}
				else
				{
					Result = Callable(*PinnedOperation, MoveTempIfPossible(LastResult));
					PinnedOperation->ExecuteNextStep();
				}
			}
		};
	}

	template <typename OpType, typename CallableType>
	void SetExecFunction(TOnlineAsyncOp<OpType>& InOperation, CallableType&& InCallable)
	{
		ExecFunction = [this, WeakOperation = TWeakPtr<TOnlineAsyncOp<OpType>>(InOperation.AsShared()), Callable = MoveTemp(InCallable)]()
		{
			TSharedPtr<TOnlineAsyncOp<OpType>> PinnedOperation = WeakOperation.Pin();
			if (PinnedOperation)
			{
				if constexpr (TOnlineAsyncOpCallableTraits<CallableType>::bRequiresPromise)
				{
					TPromise<ResultType> Promise;
					// set promise continuation before calling the callable so that we will complete the step as soon as the value is set
					Promise.GetFuture()
						.Next([this, WeakOperation](const ResultType& Value)
						{
							TSharedPtr<TOnlineAsyncOp<OpType>> PinnedOperation2 = WeakOperation.Pin();
							if (PinnedOperation2)
							{
								Result = Value;
								PinnedOperation2->ExecuteNextStep();
							}
						});

					Callable(*PinnedOperation, MoveTemp(Promise));
				}
				else if constexpr (TOnlineAsyncOpCallableTraits<CallableType>::bAsyncResult)
				{
					Callable(*PinnedOperation)
						.Next([this, WeakOperation](const ResultType& Value)
						{
							TSharedPtr<TOnlineAsyncOp<OpType>> PinnedOperation2 = WeakOperation.Pin();
							if (PinnedOperation2)
							{
								Result = Value;
								PinnedOperation2->ExecuteNextStep();
							}
						});
				}
				else
				{
					Result = Callable(*PinnedOperation);
					PinnedOperation->ExecuteNextStep();
				}
			}
		};
	}

	virtual const FOnlineAsyncExecutionPolicy& GetExecutionPolicy() const override
	{
		return ExecutionPolicy;
	}

	virtual void Execute() override
	{
		check(ExecFunction)
		ExecFunction();
	}


	ResultType& GetResultRef()
	{
		return Result;
	}

private:
	FOnlineAsyncExecutionPolicy ExecutionPolicy;
	TUniqueFunction<void()> ExecFunction;
	ResultType Result;
};


template <>
class TStep<void> : public IStep
{
public:
	TStep(FOnlineAsyncExecutionPolicy&& InExecutionPolicy)
		: ExecutionPolicy(MoveTemp(InExecutionPolicy))
	{
	}

	template <typename OpType, typename LastResultType, typename CallableType>
	void SetExecFunction(TOnlineAsyncOp<OpType>& InOperation, LastResultType&& InLastResult, CallableType&& InCallable)
	{
		ExecFunction = [this, WeakOperation = TWeakPtr<TOnlineAsyncOp<OpType>>(InOperation.AsShared()), &LastResult = InLastResult, Callable = MoveTemp(InCallable)]() mutable
		{
			TSharedPtr<TOnlineAsyncOp<OpType>> PinnedOperation = WeakOperation.Pin();
			if (PinnedOperation)
			{
				if constexpr (TOnlineAsyncOpCallableTraits<CallableType>::bAsyncResult)
				{
					Callable(*PinnedOperation, MoveTempIfPossible(LastResult))
						.Next([this, WeakOperation](const int& Value)
						{
							TSharedPtr<TOnlineAsyncOp<OpType>> PinnedOperation2 = WeakOperation.Pin();
							if (PinnedOperation2)
							{
								PinnedOperation2->ExecuteNextStep();
							}
						});
				}
				else
				{
					Callable(*PinnedOperation, MoveTempIfPossible(LastResult));
					PinnedOperation->ExecuteNextStep();
				}
			}
		};
	}

	template <typename OpType, typename CallableType>
	void SetExecFunction(TOnlineAsyncOp<OpType>& InOperation, CallableType&& InCallable)
	{
		ExecFunction = [this, WeakOperation = TWeakPtr<TOnlineAsyncOp<OpType>>(InOperation.AsShared()), Callable = MoveTemp(InCallable)]()
		{
			TSharedPtr<TOnlineAsyncOp<OpType>> PinnedOperation = WeakOperation.Pin();
			if (PinnedOperation)
			{
				if constexpr (TOnlineAsyncOpCallableTraits<CallableType>::bAsyncResult)
				{
					Callable(*PinnedOperation)
						.Next([this, WeakOperation](const int& Value)
						{
							TSharedPtr<TOnlineAsyncOp<OpType>> PinnedOperation2 = WeakOperation.Pin();
							if (PinnedOperation2)
							{
								PinnedOperation2->ExecuteNextStep();
							}
						});
				}
				else
				{
					Callable(*PinnedOperation);
					PinnedOperation->ExecuteNextStep();
				}
			}
		};
	}

	virtual const FOnlineAsyncExecutionPolicy& GetExecutionPolicy() const override
	{
		return ExecutionPolicy;
	}

	virtual void Execute() override
	{
		check(ExecFunction)
		ExecFunction();
	}

private:
	FOnlineAsyncExecutionPolicy ExecutionPolicy;
	TUniqueFunction<void()> ExecFunction;
};

// Provides Then continuation for both TOnlineAsyncOp and TOnlineChainableAsyncOp
template <typename Outer, typename OpType, typename LastResultType>
class TOnlineAsyncOpBase
{
public:
	// Callable can take one of the following forms, where the second form is used when an asynchronous
	//     call can set the promise with a value that is only valid for the duration of the Callable call
	//   ResultType(AsyncOp, LastResult)
	//     AsyncOp is a FOnlineAsyncOp& or TOnlineAsyncOp<OpType>&
	//     LastResult is a LastResultType or const LastResultType&
	//     ResultType is any type, or a TFuture to allow for an asynchronous result
	//   void(AsyncOp, LastResult, TPromise<ResultType>&&)
	//     AsyncOp is a FOnlineAsyncOp& or TOnlineAsyncOp<OpType>&
	//     LastResult is a LastResultType or const LastResultType&
	//     ResultType is any non-void type
	template <typename CallableType>
	auto Then(CallableType&& Callable, FOnlineAsyncExecutionPolicy ExecutionPolicy = FOnlineAsyncExecutionPolicy::RunOnGameThread());

protected:
	LastResultType* LastResult;
};

template <typename Outer, typename OpType>
class TOnlineAsyncOpBase<Outer, OpType, void>
{
public:
	// Callable can take one of the following forms, where the second form is used when an asynchronous
	//     call can set the promise with a value that is only valid for the duration of the Callable call
	//   ResultType(AsyncOp)
	//     AsyncOp is a FOnlineAsyncOp& or TOnlineAsyncOp<OpType>&
	//     ResultType is any type, or a TFuture to allow for an asynchronous result
	//   void(AsyncOp, TPromise<ResultType>&&)
	//     AsyncOp is a FOnlineAsyncOp& or TOnlineAsyncOp<OpType>&
	//     ResultType is any non-void type
	template <typename CallableType>
	auto Then(CallableType&& Callable, FOnlineAsyncExecutionPolicy ExecutionPolicy = FOnlineAsyncExecutionPolicy::RunOnGameThread());
};


/* Private */ }

template <typename OpType, typename T>
class TOnlineChainableAsyncOp : public Private::TOnlineAsyncOpBase<TOnlineChainableAsyncOp<OpType, T>, OpType, T>
{
public:
	TOnlineChainableAsyncOp(TOnlineAsyncOp<OpType>& InOwningOperation, const T* InLastResult)
		: OwningOperation(InOwningOperation)
		, LastResult(InLastResult)
	{
	}

	TOnlineChainableAsyncOp(TOnlineChainableAsyncOp&& Other)
		: OwningOperation(Other.OwningOperation)
		, LastResult(Other.LastResult)
	{
	}

	TOnlineChainableAsyncOp& operator=(TOnlineChainableAsyncOp&& Other)
	{
		check(&OwningOperation == &Other.OwningOperation); // Can't reassign this
		LastResult = Other.LastResult;
		return *this;
	}

	// placeholder
	template <typename... U>
	void Enqueue(U&&...)
	{
		static_assert(std::is_same_v<T, void>, "Continuation result discarded. Continuation prior to calling Enqueue must have a void or TFuture<void> return type.");
		OwningOperation.Enqueue();
	}

	TOnlineAsyncOp<OpType>& GetOwningOperation()
	{
		return OwningOperation;
	}

protected:
	TOnlineAsyncOp<OpType>& OwningOperation;
	const T* LastResult;
};

class FOnlineAsyncOp
{
public:
	virtual ~FOnlineAsyncOp() {}

	Private::FOnlineOperationData Data;

	virtual void SetError(FOnlineError&& Error) = 0;
};

// This class represents an async operation on the public interface
// There may be one or more handles pointing to one instance
template <typename OpType>
class TOnlineAsyncOp 
	: public Private::TOnlineAsyncOpBase<TOnlineAsyncOp<OpType>, OpType, void>
	, public FOnlineAsyncOp
	, public TSharedFromThis<TOnlineAsyncOp<OpType>>
{
public:
	using ParamsType = typename OpType::Params;
	using ResultType = typename OpType::Result;

	TOnlineAsyncOp(FOnlineServicesCommon& InServices, ParamsType&& Params)
		: Services(InServices)
		, SharedState(MakeShared<FAsyncOpSharedState>(MoveTemp(Params)))
	{
	}

	~TOnlineAsyncOp()
	{
	}

	bool IsReady() const
	{
		return SharedState->State != EAsyncOpState::Invalid;
	}

	bool IsComplete() const
	{
		return SharedState->State >= EAsyncOpState::Complete;
	}

	const ParamsType& GetParams() const
	{
		return SharedState->Params;
	}

	TOnlineAsyncOp<OpType>& GetOwningOperation()
	{
		return *this;
	}

	operator TOnlineChainableAsyncOp<OpType, void>()
	{
		return TOnlineChainableAsyncOp<OpType, void>(*this, nullptr);
	}

	static TOnlineAsyncOp<OpType> CreateError(const FOnlineError& Error) { return TOnlineAsyncOp<OpType>(); }

	TOnlineAsyncOpHandle<OpType> GetHandle()
	{
		return TOnlineAsyncOpHandle<OpType>(CreateSharedState());
	}

	void Cancel(const FOnlineError& Reason)
	{
		SetError(Reason);
		SharedState->State = EAsyncOpState::Cancelled;
	}

	void SetResult(ResultType&& InResult)
	{
		SharedState->Result = TOnlineResult<ResultType>(MoveTemp(InResult));
		SharedState->State = EAsyncOpState::Complete;

		TriggerOnComplete(SharedState->Result);
	}

	virtual void SetError(FOnlineError&& Error)
	{
		SharedState->Result = TOnlineResult<ResultType>(MoveTemp(Error));
		SharedState->State = EAsyncOpState::Complete;

		TriggerOnComplete(SharedState->Result);
	}

	FOnlineServicesCommon& GetServices() { return Services; }

	template <typename... U>
	void Enqueue(U&&...)
	{
		// placeholder
		SharedState->State = EAsyncOpState::Queued;
		SharedState->State = EAsyncOpState::Running;
		ExecuteNextStep();
	}

	void ExecuteNextStep()
	{
		if (!IsComplete())
		{
			if (NextStep < Steps.Num())
			{
				Execute(Steps[NextStep]->GetExecutionPolicy(),
					[this, StepToExecute = NextStep, WeakThis = TWeakPtr<TOnlineAsyncOp<OpType>>(this->AsShared())]()
					{
						TSharedPtr<TOnlineAsyncOp<OpType>> PinnedThis = WeakThis.Pin();
						if (PinnedThis)
						{
							Steps[StepToExecute]->Execute();
						}
					});
			}
			++NextStep;
		}
	}

	void AddStep(TUniquePtr<Private::IStep>&& Step)
	{
		Steps.Add(MoveTemp(Step));
	}

	template <typename CallableType>
	void Execute(FOnlineAsyncExecutionPolicy ExecutionPolicy, CallableType&& Callable)
	{
		switch (ExecutionPolicy.GetExecutionPolicy())
		{
		case EOnlineAsyncExecutionPolicy::RunOnGameThread:
			if (IsInGameThread())
			{
				Callable();
			}
			else
			{
				Async(EAsyncExecution::TaskGraphMainThread, MoveTemp(Callable));
			}
			break;

		case EOnlineAsyncExecutionPolicy::RunOnNextTick:
			Async(EAsyncExecution::TaskGraphMainThread, MoveTemp(Callable));
			break;

		case EOnlineAsyncExecutionPolicy::RunOnThreadPool:
			Async(EAsyncExecution::ThreadPool, MoveTemp(Callable));
			break;

		case EOnlineAsyncExecutionPolicy::RunOnTaskGraph:
			Async(EAsyncExecution::TaskGraph, MoveTemp(Callable));
			break;

		case EOnlineAsyncExecutionPolicy::RunImmediately:
			Callable();
			break;
		}
	}

protected:
	FOnlineServicesCommon& Services;

	void TriggerOnComplete(const TOnlineResult<ResultType>& Result)
	{
		TArray<TSharedRef<FAsyncOpSharedHandleState>> SharedHandleStatesCopy(SharedHandleStates);
		for (TSharedRef<FAsyncOpSharedHandleState>& SharedHandleState : SharedHandleStatesCopy)
		{
			SharedHandleState->TriggerOnComplete(Result);
		}

		OnCompleteEvent.Broadcast(Result);
	}

	class FAsyncOpSharedState
	{
	public:
		FAsyncOpSharedState(ParamsType&& InParams)
			: Params(MoveTemp(InParams))
		{
		}

		ParamsType Params;
		// This will need to be protected with a mutex if we want to allow this to be set from multiple threads (eg, set result from a task graph thread, while allowing this to be cancelled from the game thread)
		TOnlineResult<ResultType> Result{ Errors::Unknown() };
		EAsyncOpState State = EAsyncOpState::Invalid;

		bool IsComplete() const
		{
			return State >= EAsyncOpState::Complete;
		}
	};

	class FAsyncOpSharedHandleState : public Private::IOnlineAsyncOpSharedState<OpType>, public TSharedFromThis<FAsyncOpSharedHandleState>
	{
	public:
		FAsyncOpSharedHandleState(const TSharedRef<TOnlineAsyncOp<OpType>>& InAsyncOp)
			: SharedState(InAsyncOp->SharedState)
			, AsyncOp(InAsyncOp)
		{
		}

		~FAsyncOpSharedHandleState()
		{
			Detach();
		}

		virtual void Cancel(const FOnlineError& Reason) override
		{
			TSharedPtr<TOnlineAsyncOp<OpType>> PinnedOP = AsyncOp.Pin();
			if (PinnedOP.IsValid())
			{
				bCancelled = true;
				TriggerOnComplete(TOnlineResult<ResultType>(Reason));
			}
		}

		virtual EAsyncOpState GetState() const override
		{
			return bCancelled ? EAsyncOpState::Cancelled : SharedState->State;
		}

		virtual void SetOnProgress(TDelegate<void(const FAsyncProgress&)>&& Function) override
		{
			OnProgressFn = MoveTemp(Function);
		}

		virtual void SetOnWillRetry(TDelegate<void(TOnlineAsyncOpHandle<OpType>& Handle, const FWillRetry&)>&& Function) override
		{
			OnWillRetryFn = MoveTemp(Function);
		}

		virtual void SetOnComplete(TDelegate<void(const TOnlineResult<ResultType>&)>&& Function) override
		{
			OnCompleteFn = MoveTemp(Function);
			if (SharedState->IsComplete())
			{
				TriggerOnComplete(SharedState->Result);
			}
		}

		void TriggerOnComplete(const TOnlineResult<ResultType>& Result)
		{
			// TODO: Execute OnCompleteFn next tick on game thread
			if (OnCompleteFn.IsBound())
			{
				OnCompleteFn.ExecuteIfBound(Result);
				OnCompleteFn.Unbind();
				Detach();
			}
		}

	private:
		void Detach()
		{
			TSharedPtr<TOnlineAsyncOp<OpType>> PinnedOp = AsyncOp.Pin();
			AsyncOp.Reset();
			if (PinnedOp.IsValid())
			{
				PinnedOp->Detach(this->AsShared());
			}
		}

		TDelegate<void(const FAsyncProgress&)> OnProgressFn;
		TDelegate<void(TOnlineAsyncOpHandle<OpType>& Handle, const FWillRetry&)> OnWillRetryFn;
		TDelegate<void(const TOnlineResult<ResultType>&)> OnCompleteFn;

		bool bCancelled = false;
		TSharedRef<FAsyncOpSharedState> SharedState;
		TWeakPtr<TOnlineAsyncOp<OpType>> AsyncOp;
	};

	void Detach(const TSharedRef<FAsyncOpSharedHandleState>& SharedHandleState)
	{
		SharedHandleStates.Remove(SharedHandleState);
	}

	TSharedRef<Private::IOnlineAsyncOpSharedState<OpType>> CreateSharedState()
	{
		TSharedRef<FAsyncOpSharedHandleState> SharedHandleState = MakeShared<FAsyncOpSharedHandleState>(this->AsShared());
		SharedHandleStates.Add(SharedHandleState);
		return StaticCastSharedRef<Private::IOnlineAsyncOpSharedState<OpType>>(SharedHandleState);
	}

	TSharedRef<FAsyncOpSharedState> SharedState;
	TArray<TSharedRef<FAsyncOpSharedHandleState>> SharedHandleStates;
	TArray<TUniquePtr<Private::IStep>> Steps;
	TPromise<void> StartExecutionPromise;
	TOnlineEventCallable<void(const TOnlineResult<ResultType>&)> OnCompleteEvent;
	int NextStep = 0;

	friend class FOnlineAsyncOpCache;
};

namespace Private {

template <typename Outer, typename OpType, typename LastResultType>
template <typename CallableType>
auto TOnlineAsyncOpBase<Outer, OpType, LastResultType>::Then(CallableType&& InCallable, FOnlineAsyncExecutionPolicy ExecutionPolicy)
{
	using ResultType = typename TOnlineAsyncOpCallableTraits<CallableType>::ResultType;

	TOnlineAsyncOp<OpType>& Op = static_cast<Outer*>(this)->GetOwningOperation();

	TStep<ResultType>* Step = new TStep<ResultType>(MoveTemp(ExecutionPolicy));
	TUniquePtr<IStep> StepPtr(Step);
	Step->SetExecFunction(Op, *LastResult, MoveTemp(InCallable));

	Op.AddStep(MoveTemp(StepPtr));

	if constexpr (std::is_same_v<ResultType, void>)
	{
		return TOnlineChainableAsyncOp<OpType, ResultType>(Op, nullptr);
	}
	else
	{
		return TOnlineChainableAsyncOp<OpType, ResultType>(Op, &Step->GetResultRef());
	}
}

template <typename Outer, typename OpType>
template <typename CallableType>
auto TOnlineAsyncOpBase<Outer, OpType, void>::Then(CallableType&& InCallable, FOnlineAsyncExecutionPolicy ExecutionPolicy)
{
	using ResultType = typename TOnlineAsyncOpCallableTraits<CallableType>::ResultType;

	TOnlineAsyncOp<OpType>& Op = static_cast<Outer*>(this)->GetOwningOperation();

	TStep<ResultType>* Step = new TStep<ResultType>(MoveTemp(ExecutionPolicy));
	TUniquePtr<IStep> StepPtr(Step);
	Step->SetExecFunction(Op, MoveTemp(InCallable));

	Op.AddStep(MoveTemp(StepPtr));

	if constexpr (std::is_same_v<ResultType, void>)
	{
		return TOnlineChainableAsyncOp<OpType, ResultType>(Op, nullptr);
	}
	else
	{
		return TOnlineChainableAsyncOp<OpType, ResultType>(Op, &Step->GetResultRef());
	}
}

/* Private */ }

/* UE::Online */ }
