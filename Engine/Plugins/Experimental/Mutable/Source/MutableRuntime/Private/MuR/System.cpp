// Copyright Epic Games, Inc. All Rights Reserved.

#include "MuR/System.h"

#include "Containers/Array.h"
#include "Containers/Map.h"
#include "Containers/Set.h"
#include "HAL/IConsoleManager.h"
#include "HAL/LowLevelMemTracker.h"
#include "HAL/PlatformCrt.h"
#include "HAL/PlatformMath.h"
#include "Logging/LogCategory.h"
#include "Logging/LogMacros.h"
#include "Misc/AssertionMacros.h"
#include "MuR/CodeRunner.h"
#include "MuR/CodeVisitor.h"
#include "MuR/InstancePrivate.h"
#include "MuR/Mesh.h"
#include "MuR/Model.h"
#include "MuR/ModelPrivate.h"
#include "MuR/MutableMath.h"
#include "MuR/MutableString.h"
#include "MuR/MutableTrace.h"
#include "MuR/NullExtensionDataStreamer.h"
#include "MuR/Operations.h"
#include "MuR/Parameters.h"
#include "MuR/ParametersPrivate.h"
#include "MuR/Platform.h"
#include "MuR/Serialisation.h"
#include "MuR/SettingsPrivate.h"
#include "MuR/SystemPrivate.h"
#include "Templates/SharedPointer.h"
#include "Templates/Tuple.h"
#include "Trace/Detail/Channel.h"
#include "ProfilingDebugging/CountersTrace.h"


namespace mu
{
    static_assert( sizeof(mat4f) == 64, "UNEXPECTED_STRUCT_PACKING" );


	MUTABLE_IMPLEMENT_ENUM_SERIALISABLE(ETextureCompressionStrategy);


    //---------------------------------------------------------------------------------------------
    //---------------------------------------------------------------------------------------------
    //---------------------------------------------------------------------------------------------
    System::System(const SettingsPtr& pInSettings, ExtensionDataStreamer* DataStreamer)
    {
		LLM_SCOPE_BYNAME(TEXT("MutableRuntime"));

        SettingsPtr pSettings = pInSettings;

        if ( !pSettings )
        {
            pSettings = new Settings;
        }

        // Choose the implementation
        m_pD = new System::Private( pSettings, DataStreamer );
    }


    //---------------------------------------------------------------------------------------------
    System::~System()
    {
		LLM_SCOPE_BYNAME(TEXT("MutableRuntime"));
		MUTABLE_CPUPROFILER_SCOPE(SystemDestructor);

        check( m_pD );

        delete m_pD;
        m_pD = nullptr;
    }


    //---------------------------------------------------------------------------------------------
    System::Private* System::GetPrivate() const
    {
        return m_pD;
    }


    //---------------------------------------------------------------------------------------------
    System::Private::Private(SettingsPtr pSettings, ExtensionDataStreamer* DataStreamer)
    {
		LLM_SCOPE_BYNAME(TEXT("MutableRuntime"));

        m_pSettings = pSettings;
        m_pStreamInterface = nullptr;
        m_pImageParameterGenerator = nullptr;
        m_maxMemory = 0;
	
		m_modelCache.m_romBudget = pSettings->GetPrivate()->m_streamingCacheBytes;

		if (!DataStreamer)
		{
			DataStreamer = new NullExtensionDataStreamer();
		}
		m_ExtensionDataStreamer = DataStreamer;
	}


    //---------------------------------------------------------------------------------------------
    System::Private::~Private()
    {
		LLM_SCOPE_BYNAME(TEXT("MutableRuntime"));
		MUTABLE_CPUPROFILER_SCOPE(SystemPrivateDestructor);

        delete m_pStreamInterface;
        m_pStreamInterface = nullptr;

		delete m_ExtensionDataStreamer;
		m_ExtensionDataStreamer = nullptr;

        delete m_pImageParameterGenerator;
        m_pImageParameterGenerator = nullptr;
    }


    //---------------------------------------------------------------------------------------------
    void System::SetStreamingInterface( ModelStreamer *pInterface )
    {
		LLM_SCOPE_BYNAME(TEXT("MutableRuntime"));

        (void)pInterface;
       
        delete m_pD->m_pStreamInterface;
        m_pD->m_pStreamInterface = pInterface;
    }


    //---------------------------------------------------------------------------------------------
    void System::SetStreamingCache( uint64 bytes )
    {
		LLM_SCOPE_BYNAME(TEXT("MutableRuntime"));

        m_pD->SetStreamingCache( bytes );
    }

	
	//---------------------------------------------------------------------------------------------
	void System::ClearStreamingCache()
    {
		LLM_SCOPE_BYNAME(TEXT("MutableRuntime"));

        m_pD->ClearStreamingCache();
   }

	
    //---------------------------------------------------------------------------------------------
    void System::SetImageParameterGenerator( ImageParameterGenerator* pInterface )
    {
		LLM_SCOPE_BYNAME(TEXT("MutableRuntime"));

        delete m_pD->m_pImageParameterGenerator;
        m_pD->m_pImageParameterGenerator = pInterface;
    }


    //---------------------------------------------------------------------------------------------
    void System::SetMemoryLimit( uint32 mem )
    {
		LLM_SCOPE_BYNAME(TEXT("MutableRuntime"));

        m_pD->m_maxMemory = mem;

        // TODO: Clear cache if we are over the new limit
    }


    //---------------------------------------------------------------------------------------------
    void System::ClearCaches()
    {
		LLM_SCOPE_BYNAME(TEXT("MutableRuntime"));

        // TODO
    }

	TRACE_DECLARE_INT_COUNTER(MutableRuntime_LiveInstances, TEXT("MutableRuntime/LiveInstances"));
	TRACE_DECLARE_INT_COUNTER(MutableRuntime_Updates, TEXT("MutableRuntime/Updates"));

    //---------------------------------------------------------------------------------------------
    Instance::ID System::NewInstance( const TSharedPtr<const Model>& pModel )
    {
		LLM_SCOPE_BYNAME(TEXT("MutableRuntime"));
		MUTABLE_CPUPROFILER_SCOPE(NewInstance);

		Private::FLiveInstance instanceData;
		instanceData.m_instanceID = ++m_pD->m_lastInstanceID;
		instanceData.m_pInstance = nullptr;
		instanceData.m_pModel = pModel;
		instanceData.m_state = -1;
		instanceData.m_memory = MakeShared<FProgramCache>();
		m_pD->m_liveInstances.Add(instanceData);

		TRACE_COUNTER_SET(MutableRuntime_LiveInstances, m_pD->m_liveInstances.Num());

		return instanceData.m_instanceID;
	}


    //---------------------------------------------------------------------------------------------
    const Instance* System::BeginUpdate( Instance::ID instanceID,
                                     const ParametersPtrConst& pParams,
                                     int32 stateIndex,
                                     uint32 lodMask )
    {
		LLM_SCOPE_BYNAME(TEXT("MutableRuntime"));
		MUTABLE_CPUPROFILER_SCOPE(SystemBeginUpdate);
		TRACE_COUNTER_INCREMENT(MutableRuntime_Updates);

		if (!pParams)
		{
			UE_LOG(LogMutableCore, Error, TEXT("Invalid parameters in mutable update."));
			return nullptr;
		}

		Private::FLiveInstance* pLiveInstance = m_pD->FindLiveInstance(instanceID);
		if (!pLiveInstance)
		{
			UE_LOG(LogMutableCore, Error, TEXT("Invalid instance id in mutable update."));
			return nullptr;
		}

		m_pD->m_memory = pLiveInstance->m_memory;

		FProgram& program = pLiveInstance->m_pModel->GetPrivate()->m_program;

		bool validState = stateIndex >= 0 && stateIndex < (int)program.m_states.Num();
		if (!validState)
		{
			UE_LOG(LogMutableCore, Error, TEXT("Invalid state in mutable update."));
			return nullptr;
		}

		// This may free resources that allow us to use less memory.
		pLiveInstance->m_pInstance = nullptr;

		bool fullBuild = (stateIndex != pLiveInstance->m_state);

		pLiveInstance->m_state = stateIndex;

		// If we changed parameters that are not in this state, we need to rebuild all.
		if (!fullBuild)
		{
			fullBuild = m_pD->CheckUpdatedParameters(pLiveInstance, pParams.get(), pLiveInstance->m_updatedParameters);
		}

		// Remove cached data
		pLiveInstance->m_memory->ClearCacheLayer0();
		if (fullBuild)
		{
			pLiveInstance->m_memory->ClearCacheLayer1();
		}

		OP::ADDRESS rootAt = pLiveInstance->m_pModel->GetPrivate()->m_program.m_states[stateIndex].m_root;

		m_pD->PrepareCache(pLiveInstance->m_pModel.Get(), stateIndex);
		pLiveInstance->m_pOldParameters = pParams->Clone();

		m_pD->RunCode(pLiveInstance->m_pModel, pParams.get(), rootAt, lodMask);

		InstancePtrConst pResult = pLiveInstance->m_memory->GetInstance(FCacheAddress(rootAt, 0, 0));

		// Debug check to see if we managed the op-hit-counts correctly
		pLiveInstance->m_memory->CheckHitCountsCleared();

		pLiveInstance->m_pInstance = pResult;
		if (pResult)
		{
			pResult->GetPrivate()->m_id = pLiveInstance->m_instanceID;
		}

		m_pD->m_memory = nullptr;

		return pResult.get();
	}


	//---------------------------------------------------------------------------------------------
	ImagePtrConst System::GetImage(Instance::ID instanceID, RESOURCE_ID imageId, int32 MipsToSkip, int32 InImageLOD)
	{
		LLM_SCOPE_BYNAME(TEXT("MutableRuntime"));
		MUTABLE_CPUPROFILER_SCOPE(SystemGetImage);

		ImagePtrConst pResult;

		// Find the live instance
		Private::FLiveInstance* pLiveInstance = m_pD->FindLiveInstance(instanceID);
		check(pLiveInstance);
		m_pD->m_memory = pLiveInstance->m_memory;

		// Find the resource id in the model's resource cache
		for (const Model::Private::RESOURCE_KEY& res : pLiveInstance->m_pModel->GetPrivate()->m_generatedResources)
		{
			if (res.m_id == imageId)
			{
				pResult = m_pD->BuildImage(pLiveInstance->m_pModel,
					pLiveInstance->m_pOldParameters.get(),
					res.m_rootAddress, MipsToSkip, InImageLOD);

				// We always need to return something valid.
				if (!pResult)
				{
					pResult = new mu::Image(16, 16, 1, EImageFormat::IF_RGBA_UBYTE);
				}

				break;
			}
		}

		m_pD->m_memory = nullptr;

		return pResult;
	}


	// Temporarily make the Image DescCache clear at every image because otherwise it makes some textures 
	// not evaluate their layout and be of size 0 and 0 lods, making them incorrectly evaluate MipsToSkip
	static TAutoConsoleVariable<int32> CVarClearImageDescCache(
		TEXT("mutable.ClearImageDescCache"),
		1,
		TEXT("If different than 0, clear the image desc cache at every image."),
		ECVF_Scalability);


	//---------------------------------------------------------------------------------------------
	void System::GetImageDesc(Instance::ID instanceID, RESOURCE_ID imageId, FImageDesc& OutDesc)
	{
		LLM_SCOPE_BYNAME(TEXT("MutableRuntime"));
		MUTABLE_CPUPROFILER_SCOPE(SystemGetImageDesc);

		OutDesc = FImageDesc();

		// Find the live instance
		Private::FLiveInstance* pLiveInstance = m_pD->FindLiveInstance(instanceID);
		check(pLiveInstance);
		m_pD->m_memory = pLiveInstance->m_memory;

		// Find the resource id in the model's resource cache
		for (const Model::Private::RESOURCE_KEY& res : pLiveInstance->m_pModel->GetPrivate()->m_generatedResources)
		{
			if (res.m_id == imageId)
			{
				const mu::Model* Model = pLiveInstance->m_pModel.Get();
				const mu::FProgram& program = Model->GetPrivate()->m_program;

				int32 VarValue = CVarClearImageDescCache.GetValueOnAnyThread();
				if (VarValue != 0)
				{
					m_pD->m_memory->m_descCache.Reset();
				}

				m_pD->m_memory->m_descCache.SetNum(program.m_opAddress.Num());

				OP::ADDRESS at = res.m_rootAddress;
				mu::OP_TYPE opType = program.GetOpType(at);
				if (GetOpDataType(opType) == DT_IMAGE)
				{
					int8 executionOptions = 0;
					CodeRunner Runner(m_pD->m_pSettings, m_pD, EExecutionStrategy::MinimizeMemory, pLiveInstance->m_pModel, pLiveInstance->m_pOldParameters.get(), at, System::AllLODs, executionOptions, 0, FScheduledOp::EType::ImageDesc);
					Runner.Run();
					Runner.GetImageDescResult(OutDesc);
				}

				break;
			}
		}

		m_pD->m_memory = nullptr;
	}


    //---------------------------------------------------------------------------------------------
    MeshPtrConst System::GetMesh( Instance::ID instanceID,
                                   RESOURCE_ID meshId )
    {
		LLM_SCOPE_BYNAME(TEXT("MutableRuntime"));
		MUTABLE_CPUPROFILER_SCOPE(SystemGetMesh);

		MeshPtrConst pResult;

		// Find the live instance
		Private::FLiveInstance* pLiveInstance = m_pD->FindLiveInstance(instanceID);
		check(pLiveInstance);
		m_pD->m_memory = pLiveInstance->m_memory;

		// Find the resource id in the model's resource cache
		for (const Model::Private::RESOURCE_KEY& res : pLiveInstance->m_pModel->GetPrivate()->m_generatedResources)
		{
			if (res.m_id == meshId)
			{
				pResult = m_pD->BuildMesh(pLiveInstance->m_pModel,
					pLiveInstance->m_pOldParameters.get(),
					res.m_rootAddress);

				// If the mesh is null it means empty, but we still need to return a valid one
				if (!pResult)
				{
					pResult = new Mesh();
				}

				break;
			}
		}

		m_pD->m_memory = nullptr;
		return pResult;
	}


    //---------------------------------------------------------------------------------------------
    void System::EndUpdate( Instance::ID instanceID )
    {
		LLM_SCOPE_BYNAME(TEXT("MutableRuntime"));
		MUTABLE_CPUPROFILER_SCOPE(EndUpdate);

		// Reduce the cache until it fits the limit.
		uint64 totalMemory = m_pD->m_modelCache.EnsureCacheBelowBudget(0, [](const Model*, int) {return false;});

		Private::FLiveInstance* pLiveInstance = m_pD->FindLiveInstance(instanceID);
		if (pLiveInstance)
		{
			pLiveInstance->m_pInstance = nullptr;
			
			// Debug check to see if we managed the op-hit-counts correctly
			pLiveInstance->m_memory->CheckHitCountsCleared();

			// We don't want to clear the cache layer 1 because it contains data that can be useful for a 
			// future update (same states, just runtime parameters changed).
			//pLiveInstance->m_memory->ClearCacheLayer1();

			// We need to clear the layer 0 cache, because it contains data that is only valid for the current 
			// parameter values (unless it is data marked as state cache)
			pLiveInstance->m_memory->ClearCacheLayer0();
		}
	}


    //---------------------------------------------------------------------------------------------
    void System::ReleaseInstance( Instance::ID instanceID )
    {
		LLM_SCOPE_BYNAME(TEXT("MutableRuntime"));
		MUTABLE_CPUPROFILER_SCOPE(ReleaseInstance);

		int Removed = m_pD->m_liveInstances.RemoveAllSwap(
			[instanceID](const Private::FLiveInstance& Instance)
			{
				return (Instance.m_instanceID == instanceID);
			});

		TRACE_COUNTER_SET(MutableRuntime_LiveInstances, m_pD->m_liveInstances.Num());

	}


    //---------------------------------------------------------------------------------------------
    class RelevantParameterVisitor : public UniqueDiscreteCoveredCodeVisitor<>
    {
    public:

        RelevantParameterVisitor
            (
                System::Private* pSystem,
				const TSharedPtr<const Model>& pModel,
                const Ptr<const Parameters>& pParams,
                bool* pFlags
            )
            : UniqueDiscreteCoveredCodeVisitor<>( pSystem, pModel, pParams, 0xffffffff )
        {
            m_pFlags = pFlags;

            FMemory::Memset( pFlags, 0, sizeof(bool)*pParams->GetCount() );

            OP::ADDRESS at = pModel->GetPrivate()->m_program.m_states[0].m_root;

            Run( at );
        }


        bool Visit( OP::ADDRESS at, FProgram& program ) override
        {
            switch ( program.GetOpType(at) )
            {
            case OP_TYPE::BO_PARAMETER:
            case OP_TYPE::NU_PARAMETER:
            case OP_TYPE::SC_PARAMETER:
            case OP_TYPE::CO_PARAMETER:
            case OP_TYPE::PR_PARAMETER:
            case OP_TYPE::IM_PARAMETER:
            {
				OP::ParameterArgs args = program.GetOpArgs<OP::ParameterArgs>(at);
				OP::ADDRESS param = args.variable;
                m_pFlags[param] = true;
                break;
            }

            default:
                break;

            }

            return UniqueDiscreteCoveredCodeVisitor<>::Visit( at, program );
        }

    private:

        //! Non-owned result buffer
        bool* m_pFlags;
    };


    //---------------------------------------------------------------------------------------------
    void System::GetParameterRelevancy( Instance::ID InstanceID,
                                        const ParametersPtrConst& Parameters,
                                        bool* Flags )
    {
		LLM_SCOPE_BYNAME(TEXT("MutableRuntime"));

		// Find the live instance
		Private::FLiveInstance* pLiveInstance = m_pD->FindLiveInstance(InstanceID);
		check(pLiveInstance);
		m_pD->m_memory = pLiveInstance->m_memory;
		
		RelevantParameterVisitor visitor( m_pD, pLiveInstance->m_pModel, Parameters, Flags );

		m_pD->m_memory = nullptr;
    }


    //---------------------------------------------------------------------------------------------
    //---------------------------------------------------------------------------------------------
    //---------------------------------------------------------------------------------------------
    bool System::Private::CheckUpdatedParameters( const FLiveInstance* LiveInstance,
                                 const Ptr<const Parameters>& Params,
                                 uint64& UpdatedParameters)
    {
        bool bFullBuild = false;

		if (!LiveInstance->m_pOldParameters)
		{
			UpdatedParameters = AllParametersMask;
			return true;
		}

        // check what parameters have changed
		UpdatedParameters = 0;
        const FProgram& program = LiveInstance->m_pModel->GetPrivate()->m_program;
        const TArray<int>& runtimeParams = program.m_states[ LiveInstance->m_state ].m_runtimeParameters;

        check( Params->GetCount() == (int)program.m_parameters.Num() );
        check( !LiveInstance->m_pOldParameters
			||
			Params->GetCount() == LiveInstance->m_pOldParameters->GetCount() );

        for ( int32 p=0; p<program.m_parameters.Num() && !bFullBuild; ++p )
        {
            bool isRuntime = runtimeParams.Contains( p );
            bool changed = !Params->HasSameValue( p, LiveInstance->m_pOldParameters, p );

            if (changed && isRuntime)
            {
				uint64 runtimeIndex = runtimeParams.IndexOfByKey(p);
				UpdatedParameters |= uint64(1) << runtimeIndex;
            }
            else if (changed)
            {
                // A non-runtime parameter has changed, we need a full build.
                // TODO: report, or log somehow.
				bFullBuild = true;
                UpdatedParameters = AllParametersMask;
            }
        }

        return bFullBuild;
    }


	//---------------------------------------------------------------------------------------------
	void System::Private::SetStreamingCache(uint64 bytes)
	{
		m_modelCache.m_romBudget = bytes;
		m_modelCache.EnsureCacheBelowBudget(0);
	}


	//---------------------------------------------------------------------------------------------
	void System::Private::ClearStreamingCache()
	{
		for (FModelCache::FModelCacheEntry& ModelCache : m_modelCache.m_cachePerModel)
	    {
		    if (const TSharedPtr<const Model> CacheModel = ModelCache.m_pModel.Pin())
			{
				FProgram& Program = CacheModel->GetPrivate()->m_program;

				for (int32 RomIndex=0; RomIndex < Program.m_roms.Num(); ++RomIndex)
				{
					Program.UnloadRom(RomIndex);		
    			}
			}
 		}
	}
	

	//---------------------------------------------------------------------------------------------
	void System::Private::BeginBuild(const TSharedPtr<const Model>& pModel)
	{
		// We don't have a FLiveInstance, let's create the memory
		// \TODO: There is no clear moment to remove this... EndBuild?
		m_memory = MakeShared<FProgramCache>();
		m_memory->Init(pModel->GetPrivate()->m_program.m_opAddress.Num());

		// Remove previously results cached from previous builds.
		m_memory->ClearCacheLayer0();

		PrepareCache(pModel.Get(), -1);
	}


	//---------------------------------------------------------------------------------------------
	void System::Private::EndBuild()
	{
		m_memory = nullptr;
	}


	//---------------------------------------------------------------------------------------------
	void System::Private::RunCode(const TSharedPtr<const Model>& InModel,
		const Parameters* InParameters, OP::ADDRESS InCodeRoot, uint32 InLODs, uint8 executionOptions, int32 InImageLOD)
	{
		CodeRunner Runner(m_pSettings, this, EExecutionStrategy::MinimizeMemory, InModel, InParameters, InCodeRoot, InLODs,
			executionOptions, InImageLOD, FScheduledOp::EType::Full);
		Runner.Run();
		bUnrecoverableError = Runner.bUnrecoverableError;
	}


	//---------------------------------------------------------------------------------------------
	bool System::Private::BuildBool(const TSharedPtr<const Model>& pModel,
		const Parameters* pParams,
		OP::ADDRESS at)
	{
		RunCode(pModel, pParams, at);
		if (bUnrecoverableError)
		{
			return false;
		}
		return m_memory->GetBool(FCacheAddress(at, 0, 0));
	}


	//---------------------------------------------------------------------------------------------
	float System::Private::BuildScalar(const TSharedPtr<const Model>& pModel,
		const Parameters* pParams,
		OP::ADDRESS at)
	{
		RunCode(pModel, pParams, at);
		if (bUnrecoverableError)
		{
			return 0.0f;
		}
		return m_memory->GetScalar(FCacheAddress(at, 0, 0));
	}


	//---------------------------------------------------------------------------------------------
	int System::Private::BuildInt(const TSharedPtr<const Model>& pModel,
		const Parameters* pParams,
		OP::ADDRESS at)
	{
		RunCode(pModel, pParams, at);
		if (bUnrecoverableError)
		{
			return 0;
		}

		return m_memory->GetInt(FCacheAddress(at, 0, 0));
	}


	//---------------------------------------------------------------------------------------------
	void System::Private::BuildColour(const TSharedPtr<const Model>& pModel,
		const Parameters* pParams,
		OP::ADDRESS at,
		float* pR,
		float* pG,
		float* pB,
		float* pA)
	{
		FVector4f col;

		mu::OP_TYPE opType = pModel->GetPrivate()->m_program.GetOpType(at);
		if (GetOpDataType(opType) == DT_COLOUR)
		{
			RunCode(pModel, pParams, at);
			if (bUnrecoverableError)
			{
				if (pR) *pR = 0.0f;
				if (pG) *pG = 0.0f;
				if (pB) *pB = 0.0f;
				if (pA) *pA = 1.0f;
			}

			col = m_memory->GetColour(FCacheAddress(at, 0, 0));
		}

		if (pR) *pR = col[0];
		if (pG) *pG = col[1];
		if (pB) *pB = col[2];
		if (pA) *pA = col[3];
	}

	
	//---------------------------------------------------------------------------------------------
	Ptr<const Projector> System::Private::BuildProjector(const TSharedPtr<const Model>& pModel, const Parameters* pParams, OP::ADDRESS at)
	{
    	RunCode(pModel, pParams, at);
		if (bUnrecoverableError)
		{
			return nullptr;
		}
    	return m_memory->GetProjector(FCacheAddress(at, 0, 0));
	}

	
	//---------------------------------------------------------------------------------------------
	Ptr<const Image> System::Private::BuildImage(const TSharedPtr<const Model>& pModel,
		const Parameters* pParams,
		OP::ADDRESS at, int32 MipsToSkip, int32 InImageLOD)
	{
		mu::OP_TYPE opType = pModel->GetPrivate()->m_program.GetOpType(at);
		if (GetOpDataType(opType) == DT_IMAGE)
		{
			RunCode(pModel, pParams, at, System::AllLODs, uint8(MipsToSkip), InImageLOD);
			if (bUnrecoverableError)
			{
				return nullptr;
			}
			ImagePtrConst Result = m_memory->GetImage(FCacheAddress(at, 0, MipsToSkip));

			// Debug check to see if we managed the op-hit-counts correctly
			m_memory->CheckHitCountsCleared();

			return Result;
		}

		return nullptr;
	}


	//---------------------------------------------------------------------------------------------
	MeshPtrConst System::Private::BuildMesh(const TSharedPtr<const Model>& pModel,
		const Parameters* pParams,
		OP::ADDRESS at)
	{
		mu::OP_TYPE opType = pModel->GetPrivate()->m_program.GetOpType(at);
		if (GetOpDataType(opType) == DT_MESH)
		{
			RunCode(pModel, pParams, at);
			if (bUnrecoverableError)
			{
				return nullptr;
			}
			MeshPtrConst pResult = m_memory->GetMesh(FCacheAddress(at, 0, 0));

			// Debug check to see if we managed the op-hit-counts correctly
			m_memory->CheckHitCountsCleared();

			return pResult;
		}

		return nullptr;
	}


	//---------------------------------------------------------------------------------------------
	LayoutPtrConst System::Private::BuildLayout(const TSharedPtr<const Model>& pModel,
		const Parameters* pParams,
		OP::ADDRESS at)
	{
		LayoutPtrConst  pResult;

		if (pModel->GetPrivate()->m_program.m_states[0].m_root)
		{
			mu::OP_TYPE opType = pModel->GetPrivate()->m_program.GetOpType(at);
			if (GetOpDataType(opType) == DT_LAYOUT)
			{
				RunCode(pModel, pParams, at);
				if (bUnrecoverableError)
				{
					return nullptr;
				}
				pResult = m_memory->GetLayout(FCacheAddress(at, 0, 0));
			}
		}

		return pResult;
	}


	//---------------------------------------------------------------------------------------------
	Ptr<const String> System::Private::BuildString(const TSharedPtr<const Model>& pModel,
		const Parameters* pParams,
		OP::ADDRESS at)
	{
		Ptr<const String> pResult;

		if (pModel->GetPrivate()->m_program.m_states[0].m_root)
		{
			mu::OP_TYPE opType = pModel->GetPrivate()->m_program.GetOpType(at);
			if (GetOpDataType(opType) == DT_STRING)
			{
				RunCode(pModel, pParams, at);
				if (bUnrecoverableError)
				{
					return nullptr;
				}
				pResult = m_memory->GetString(FCacheAddress(at, 0, 0));
			}
		}

		return pResult;
	}


	//---------------------------------------------------------------------------------------------
	void System::Private::PrepareCache( const Model* pModel, int state)
	{
		MUTABLE_CPUPROFILER_SCOPE(PrepareCache);

		FProgram& program = pModel->GetPrivate()->m_program;
		size_t opCount = program.m_opAddress.Num();
		m_memory->m_opHitCount.clear();
		m_memory->Init(opCount);

		// Mark the resources that have to be cached to update the instance in this state
		if (state >= 0)
		{
			const FProgram::FState& s = program.m_states[state];
			for (uint32 a : s.m_updateCache)
			{
				m_memory->SetForceCached(a);
			}
		}
	}


    //---------------------------------------------------------------------------------------------
    //---------------------------------------------------------------------------------------------
    //---------------------------------------------------------------------------------------------
	FModelCache::FModelCacheEntry& FModelCache::GetModelCache(const TSharedPtr<const Model>& InModel )
    {
        check(InModel);

        for(FModelCacheEntry& c:m_cachePerModel)
        {
			TSharedPtr<const Model> pCandidate = c.m_pModel.Pin();
            if (pCandidate)
            {
                if (pCandidate==InModel)
                {
                    return c;
                }
            }
            else
            {
                // Free stray data. TODO: remove vector entry.
                c.m_romWeight.Empty();
            }
        }

        // Not found. Add new
		FModelCacheEntry n;
        n.m_pModel = TWeakPtr<const Model>(InModel);
        m_cachePerModel.Add(n);
        return m_cachePerModel.Last();
    }

	/** */
	TRACE_DECLARE_INT_COUNTER(MutableRuntime_StreamingBytes, TEXT("MutableRuntime/StreamingBytes"));

    //---------------------------------------------------------------------------------------------
    uint64 FModelCache::EnsureCacheBelowBudget( uint64 additionalMemory,
												TFunctionRef<bool(const Model*,int)> isRomLockedFunc )
    {

		uint64 totalMemory = 0;
        for (FModelCacheEntry& m : m_cachePerModel)
        {
			TSharedPtr<const Model> pCacheModel = m.m_pModel.Pin();
            if (pCacheModel)
            {
				mu::FProgram& program = pCacheModel->GetPrivate()->m_program;
				for (int32 RomIndex = 0; RomIndex < program.m_roms.Num(); ++RomIndex)
				{
					if (program.IsRomLoaded(RomIndex))
					{
						totalMemory += program.m_roms[RomIndex].Size;
					}
				}
			}
        }

        if (totalMemory>0)
        {
            totalMemory += additionalMemory;

            bool finished = totalMemory < m_romBudget;
            while (!finished)
            {
				TSharedPtr<const Model> lowestPriorityModel;
                int32 lowestPriorityRom = -1;
                float lowestPriority = 0.0f;
                for (FModelCacheEntry& modelCache : m_cachePerModel)
                {
					TSharedPtr<const Model> pCacheModel = modelCache.m_pModel.Pin();
                    if (pCacheModel)
                    {
						mu::FProgram& program = pCacheModel->GetPrivate()->m_program;
                        check( modelCache.m_romWeight.Num() == program.m_roms.Num());

                        for (int32 RomIndex=0; RomIndex <program.m_roms.Num(); ++RomIndex)
                        {
							const FRomData& Rom = program.m_roms[RomIndex];
							bool bIsLoaded = program.IsRomLoaded(RomIndex);

                            if (bIsLoaded
                                &&
                                (!isRomLockedFunc(pCacheModel.Get(),RomIndex)) )
                            {
                                constexpr float factorWeight = 100.0f;
                                constexpr float factorTime = -1.0f;
                                float priority =
                                        factorWeight * float(modelCache.m_romWeight[RomIndex].Key)
                                        +
                                        factorTime * float((m_romTick-modelCache.m_romWeight[RomIndex].Value));

                                if (lowestPriorityRom<0 || priority<lowestPriority)
                                {
                                    lowestPriorityRom = RomIndex;
                                    lowestPriority = priority;
                                    lowestPriorityModel = pCacheModel;
                                }
                            }
                        }
                    }
                }

                if (lowestPriorityRom<0)
                {
                    // None found
                    finished = true;

					// If we reached this it means we need to use more memory for streaming data that was given to mutable.
					// Try to continue anyway.
                    //UE_LOG(LogMutableCore,Log, TEXT("EnsureCacheBelowBudget failed to free all the necessary memory."));
                }
                else
                {
                    //UE_LOG(LogMutableCore,Log, "Unloading rom because of memory budget: %d.", lowestPriorityRom);
					const FRomData& Rom = lowestPriorityModel->GetPrivate()->m_program.m_roms[lowestPriorityRom];
                    lowestPriorityModel->GetPrivate()->m_program.UnloadRom(lowestPriorityRom);
                    totalMemory -= lowestPriorityModel->GetPrivate()->m_program.m_roms[lowestPriorityRom].Size;
                    finished = totalMemory < m_romBudget;
                }
            }
        }

		TRACE_COUNTER_SET(MutableRuntime_StreamingBytes, totalMemory);

        return totalMemory;
    }


    //---------------------------------------------------------------------------------------------
    void FModelCache::MarkRomUsed( int romIndex, const TSharedPtr<const Model>& pModel )
    {
        check(pModel);

        FProgram& program = pModel->GetPrivate()->m_program;

        // If budget is zero, we don't unload anything here, and we assume it is managed
        // somewhere else.
        if ( !m_romBudget )
        {
            return;
        }

        ++m_romTick;

        // Update current cache
        {
			FModelCacheEntry& modelCache = GetModelCache(pModel);

            while (modelCache.m_romWeight.Num()<program.m_roms.Num())
            {
				modelCache.m_romWeight.Add({ 0,0 });
            }

            modelCache.m_romWeight[romIndex].Key++;
            modelCache.m_romWeight[romIndex].Value = m_romTick;
        }
    }


    //---------------------------------------------------------------------------------------------
    void FModelCache::UpdateForLoad( int romIndex, const TSharedPtr<const Model>& pModel,
                                     TFunctionRef<bool(const Model*,int)> isRomLockedFunc )
    {
		MarkRomUsed( romIndex, pModel);

		FProgram& program = pModel->GetPrivate()->m_program;
        EnsureCacheBelowBudget(program.m_roms[romIndex].Size, isRomLockedFunc);
    }

}
