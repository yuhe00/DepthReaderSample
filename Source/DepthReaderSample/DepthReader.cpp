#include "DepthReader.h"

#include "CommonRenderResources.h"
#include "EngineModule.h"
#include "ScreenRendering.h"

DECLARE_CYCLE_STAT(TEXT("DepthReader"), STAT_DepthReader, STATGROUP_Tickables);
DECLARE_GPU_STAT_NAMED(DepthReader, TEXT("Depth Reader"));

ADepthReader::ADepthReader()
{
	PrimaryActorTick.bCanEverTick = true;

	USceneComponent* Root = CreateDefaultSubobject<USceneComponent>(TEXT("RootComponent"));
	SetRootComponent(Root);
	AddOwnedComponent(Root);
}

void ADepthReader::BeginPlay()
{
	Super::BeginPlay();

	PostOpaqueRenderDelegate.BindUObject(this, &ADepthReader::UpdateCaptureDepthTexture_RenderThread);
	UpdateCaptureDepthTextureHandle = GetRendererModule().RegisterPostOpaqueRenderDelegate(PostOpaqueRenderDelegate);
}

void ADepthReader::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	GetRendererModule().RemovePostOpaqueRenderDelegate(UpdateCaptureDepthTextureHandle);

	ENQUEUE_RENDER_COMMAND(DepthReaderReleaseTextures)(
		[this](FRHICommandListImmediate& RHICmdList)
		{
			for (int i = 0; i < 2; i++)
			{
				if (ReadbackTextures[i] && ReadbackTextures[i].IsValid())
				{
					ReadbackTextures[i].SafeRelease();
				}

				if (IntermediateTextures[i] && IntermediateTextures[i].IsValid())
				{
					IntermediateTextures[i].SafeRelease();
				}
			}
		});

	FlushRenderingCommands();

	Super::EndPlay(EndPlayReason);
}

void ADepthReader::Tick(float DeltaSeconds)
{
	SCOPE_CYCLE_COUNTER(STAT_DepthReader);
	
	UpdateCaptureDepthTexture();
	
	// TODO Only calculate when needed
	{
		ULocalPlayer* LocalPlayer = GetGameInstance()->GetFirstLocalPlayerController()->GetLocalPlayer();
		FSceneViewFamilyContext ViewFamily(
			FSceneViewFamily::ConstructionValues(
				LocalPlayer->ViewportClient->Viewport,
				GetWorld()->Scene,
				LocalPlayer->ViewportClient->EngineShowFlags
			).SetRealtimeUpdate(true));
		FVector ViewLocation;
		FRotator ViewRotation;
		const FSceneView* SceneView = LocalPlayer->CalcSceneView(&ViewFamily, ViewLocation, ViewRotation,
		                                                         LocalPlayer->ViewportClient->Viewport);
		InvDeviceZToWorldZTransform = SceneView->InvDeviceZToWorldZTransform;
	}
}

FVector ADepthReader::GetWorldSpaceMousePosition()
{
	FVector WorldPosition, WorldDirection;
	const APlayerController* PlayerController = GetWorld()->GetFirstPlayerController();
	PlayerController->DeprojectMousePositionToWorld(WorldPosition, WorldDirection);

	const ULocalPlayer* LocalPlayer = Cast<ULocalPlayer>(PlayerController->Player);
	FIntPoint MousePos;
	LocalPlayer->ViewportClient->Viewport->GetMousePos(MousePos);
	FMinimalViewInfo ViewInfo;
	GetWorld()->GetFirstPlayerController()->CalcCamera(0.f, ViewInfo);

	const FVector ForwardVector = ViewInfo.Rotation.RotateVector(FVector::ForwardVector);
	const float Distance = GetDepth(MousePos) / FVector::DotProduct(ForwardVector, WorldDirection);

	return WorldPosition + WorldDirection * Distance;
}

float ADepthReader::GetDepth(const FIntPoint& MousePos)
{
	const FIntPoint Coord = GetBufferPosFromMousePos(MousePos);
	return SampleDepth(Coord.X, Coord.Y);
}

float ADepthReader::SampleDepth(int32 Col, int32 Row)
{
	const size_t Index = Col + Row * CachedAlignedReadbackDataSize.X;
	if (Index < CurrentAlignedReadbackData.Num())
	{
		const float DeviceZ = CurrentAlignedReadbackData[Index].Depth;
		const float SampledDepth = ConvertDeviceZToDepth(DeviceZ);
		LastDepth = SampledDepth;
	}
	else
	{
		UE_LOG(LogTemp, Warning, TEXT("Trying to sample from (%d, %d) - Out of bounds"), Col, Row);
	}

	return LastDepth;
}

FIntPoint ADepthReader::GetBufferPosFromMousePos(const FIntPoint& MousePos) const
{
	const APlayerController* PlayerController = GetWorld()->GetFirstPlayerController();
	const ULocalPlayer* LocalPlayer = Cast<ULocalPlayer>(PlayerController->Player);
	const FViewport* Viewport = LocalPlayer->ViewportClient->Viewport;

	const float X = static_cast<float>(MousePos.X) / Viewport->GetSizeXY().X;
	const float Y = static_cast<float>(MousePos.Y) / Viewport->GetSizeXY().Y;
	const float ScaledX = X * CachedReadbackTextureSize.X * CachedCoordScale.X;
	const float ScaledY = Y * CachedReadbackTextureSize.Y * CachedCoordScale.Y;
	const int MouseX = FMath::Clamp(static_cast<int>(ScaledX), 0, CachedReadbackTextureSize.X - 1);
	const int MouseY = FMath::Clamp(static_cast<int>(ScaledY), 0, CachedReadbackTextureSize.Y - 1);

	return FIntPoint(MouseX, MouseY);
}

float ADepthReader::ConvertDeviceZToDepth(float DeviceZ) const
{
	return (DeviceZ * InvDeviceZToWorldZTransform[0] + InvDeviceZToWorldZTransform[1] + 1.0f
		/ (DeviceZ * InvDeviceZToWorldZTransform[2] - InvDeviceZToWorldZTransform[3])) - GNearClippingPlane;
}

void ADepthReader::UpdateCaptureDepthTexture()
{
	if ((!ReadDepthFence || !ReadDepthFence.IsValid() ||
		(ReadDepthFence->NumPendingWriteCommands.GetValue() == 0 || ReadDepthFence->Poll())))
	{
		{
			FScopeLock Lock(&CriticalSection);

			CurrentAlignedReadbackData.SetNumUninitialized(AlignedReadbackData[CurrentRenderIndex].Num());
			FMemory::Memcpy(CurrentAlignedReadbackData.GetData(), AlignedReadbackData[CurrentRenderIndex].GetData(),
			                CurrentAlignedReadbackData.Num() * sizeof(FDepthPixel));

			CachedCoordScale = FVector2D(
				static_cast<float>(EffectiveSize.Width()) / DepthBufferSize.X,
				static_cast<float>(EffectiveSize.Height()) / DepthBufferSize.Y
			);
			CachedReadbackTextureSize = ReadbackTextureSize;
			CachedAlignedReadbackDataSize = AlignedReadbackDataSize;

			CurrentRenderIndex = (CurrentRenderIndex + 1) % 2;
		}

		// Debug stuff

		if (bDebugOutput && CachedAlignedReadbackDataSize.Size() > 0)
		{
			TArray<FColor> Colors;
			for (int i = 0; i < CurrentAlignedReadbackData.Num(); i++)
			{
				const float DeviceZ = CurrentAlignedReadbackData[i].Depth;
				const float SampledDepth = ConvertDeviceZToDepth(DeviceZ);
				const uint8 V = static_cast<uint8>(SampledDepth / 1000.f);
				Colors.Add(FColor(V, V, V));
			}

			const FString SavedDir = FPaths::ProjectSavedDir();
			const FString Filename = FString::Printf(TEXT("%s/DepthReaderDebug/Output"), *SavedDir);

			if (AlignedReadbackDataSize.Size() > 0 && Colors.GetData())
			{
				FFileHelper::CreateBitmap(
					*Filename,
					CachedAlignedReadbackDataSize.X, CachedAlignedReadbackDataSize.Y,
					Colors.GetData()
				);
			}
		}
	}
}

void ADepthReader::UpdateCaptureDepthTexture_RenderThread(FPostOpaqueRenderParameters& PostOpaqueRenderParameters)
{
	FRHICommandListImmediate& RHICmdList = *PostOpaqueRenderParameters.RHICmdList;

	SCOPED_DRAW_EVENT(RHICmdList, DepthReader);
	SCOPED_GPU_STAT(RHICmdList, DepthReader);

	CriticalSection.Lock();
	const int RenderIndex = CurrentRenderIndex;
	EffectiveSize = PostOpaqueRenderParameters.ViewportRect;
	CriticalSection.Unlock();

	CheckDepthTexture_RenderThread(RHICmdList, PostOpaqueRenderParameters.DepthTexture);

	if (!bInitialized)
	{
		CopyDepthToResolve_RenderThread(RHICmdList, PostOpaqueRenderParameters.DepthTexture, RenderIndex);
		bInitialized = true;
	}
	else if (CopyResolveFence->NumPendingWriteCommands.GetValue() == 0 || CopyResolveFence->Poll())
	{
		ReadDepthTexture_RenderThread(RHICmdList, RenderIndex);
		CopyDepthToResolve_RenderThread(RHICmdList, PostOpaqueRenderParameters.DepthTexture, RenderIndex);
	}
}

void ADepthReader::CheckDepthTexture_RenderThread(FRHICommandListImmediate& RHICmdList,
                                                  const FRHITexture2D* DepthTexture)
{
	FScopeLock Lock(&CriticalSection);

	if (!CopyResolveFence || !CopyResolveFence.IsValid())
	{
		CopyResolveFence = RHICmdList.CreateGPUFence("CopyResolveFence");
	}

	if (!ReadDepthFence || !ReadDepthFence.IsValid())
	{
		ReadDepthFence = RHICmdList.CreateGPUFence("ReadDepthFence");
	}

	// Resize readback textures if depth texture has changed

	DepthBufferSize = DepthTexture->GetSizeXY();

	const float ResX = static_cast<float>(DepthBufferSize.X) * ResolutionFraction;
	const float ResY = static_cast<float>(DepthBufferSize.Y) * ResolutionFraction;
	const FIntPoint DesiredReadbackTextureSize(ResX, ResY);

	if (ReadbackTextureSize != DesiredReadbackTextureSize)
	{
		ReadbackTextureSize = DesiredReadbackTextureSize;

		for (int i = 0; i < 2; i++)
		{
			if (ReadbackTextures[i] && ReadbackTextures[i].IsValid())
			{
				ReadbackTextures[i].SafeRelease();
			}

			if (IntermediateTextures[i] && IntermediateTextures[i].IsValid())
			{
				IntermediateTextures[i].SafeRelease();
			}

			{
				FRHIResourceCreateInfo CreateInfo;
				ReadbackTextures[i] = RHICreateTexture2D(ReadbackTextureSize.X, ReadbackTextureSize.Y, PF_R32_FLOAT, 1,
				                                         1, TexCreate_CPUReadback | TexCreate_HideInVisualizeTexture,
				                                         ERHIAccess::ResolveDst, CreateInfo);
			}

			{
				FRHIResourceCreateInfo CreateInfo;
				IntermediateTextures[i] = RHICreateTexture2D(ReadbackTextureSize.X, ReadbackTextureSize.Y, PF_R32_FLOAT,
				                                             1, 1, TexCreate_RenderTargetable, CreateInfo);
			}

			RHICmdList.ImmediateFlush(EImmediateFlushType::FlushRHIThread);

			// Mapping staging surface requires HW-dependent data alignment

			void* StageBuffer;
			int Width, Height;
			RHICmdList.MapStagingSurface(ReadbackTextures[i], StageBuffer, Width, Height);

			// Hacky workaround for ComputeBytesPerPixel returning 5 instead of 8 for Depth pixel on DX11
			// No longer in use since we are not sampling directly from R32G824_TYPELESS readback
			// Width = (Width * 5) >> 3;
			// Width += Width % 2 == 0 ? 0 : 1;

			AlignedReadbackData[i].SetNumUninitialized(Width * Height, false);
			AlignedReadbackDataSize = FIntPoint(Width, Height);

			RHICmdList.UnmapStagingSurface(ReadbackTextures[i]);

			RHICmdList.ImmediateFlush(EImmediateFlushType::FlushRHIThread);
		}

		UE_LOG(LogTemp, Log, TEXT("Created new readback texture and buffer: %dx%d (%dx%d)"),
		       ReadbackTextureSize.X,
		       ReadbackTextureSize.Y,
		       AlignedReadbackDataSize.X,
		       AlignedReadbackDataSize.Y);
	}
}

void ADepthReader::CopyDepthToResolve_RenderThread(FRHICommandListImmediate& RHICmdList, FRHITexture2D* DepthTexture,
                                                   const int RenderIndex) const
{
	CopyResolveFence->Clear();

	const FTexture2DRHIRef& IntermediateTexture = IntermediateTextures[RenderIndex];
	const FTexture2DRHIRef& ReadbackTexture = ReadbackTextures[RenderIndex];

	// Resample depth to intermediate texture
	{
		IRendererModule* RendererModule = &FModuleManager::GetModuleChecked<IRendererModule>("Renderer");

		const FRHIRenderPassInfo RenderPassInfo(IntermediateTexture, ERenderTargetActions::Load_Store);
		RHICmdList.BeginRenderPass(RenderPassInfo, TEXT("DepthReaderResampleDepth"));

		{
			RHICmdList.SetViewport(0, 0, 0.0f, IntermediateTexture->GetSizeX(), IntermediateTexture->GetSizeY(), 1.0f);

			FGraphicsPipelineStateInitializer GraphicsPSOInit;
			RHICmdList.ApplyCachedRenderTargets(GraphicsPSOInit);
			GraphicsPSOInit.BlendState = TStaticBlendState<>::GetRHI();
			GraphicsPSOInit.RasterizerState = TStaticRasterizerState<>::GetRHI();
			GraphicsPSOInit.DepthStencilState = TStaticDepthStencilState<false, CF_Always>::GetRHI();

			const FGlobalShaderMap* ShaderMap = GetGlobalShaderMap(GMaxRHIFeatureLevel);
			const TShaderMapRef<FScreenVS> VertexShader(ShaderMap);
			const TShaderMapRef<FScreenPS> PixelShader(ShaderMap);

			GraphicsPSOInit.BoundShaderState.VertexDeclarationRHI = GFilterVertexDeclaration.VertexDeclarationRHI;
			GraphicsPSOInit.BoundShaderState.VertexShaderRHI = VertexShader.GetVertexShader();
			GraphicsPSOInit.BoundShaderState.PixelShaderRHI = PixelShader.GetPixelShader();

			GraphicsPSOInit.PrimitiveType = PT_TriangleList;

			SetGraphicsPipelineState(RHICmdList, GraphicsPSOInit);

			if (IntermediateTexture->GetSizeX() != DepthTexture->GetSizeX() ||
				IntermediateTexture->GetSizeY() != DepthTexture->GetSizeY())
			{
				PixelShader->SetParameters(RHICmdList, TStaticSamplerState<SF_Bilinear>::GetRHI(), DepthTexture);
			}
			else
			{
				PixelShader->SetParameters(RHICmdList, TStaticSamplerState<SF_Point>::GetRHI(), DepthTexture);
			}

			RendererModule->DrawRectangle(RHICmdList, 0, 0, // Dest X, Y
			                              IntermediateTexture->GetSizeX(), // Dest Width
			                              IntermediateTexture->GetSizeY(), // Dest Height
			                              0, 0, // Source U, V
			                              1, 1, // Source USize, VSize
			                              IntermediateTexture->GetSizeXY(), // Target buffer size
			                              FIntPoint(1, 1), // Source texture size
			                              VertexShader, EDRF_Default);
		}

		RHICmdList.EndRenderPass();
	}

	RHICmdList.CopyToResolveTarget(
		IntermediateTexture,
		ReadbackTexture,
		FResolveParams());

	RHICmdList.Transition(FRHITransitionInfo(ReadbackTexture, ERHIAccess::ResolveDst, ERHIAccess::CPURead));
	RHICmdList.WriteGPUFence(CopyResolveFence);
}

void ADepthReader::ReadDepthTexture_RenderThread(FRHICommandListImmediate& RHICmdList, const int RenderIndex)
{
	ReadDepthFence->Clear();

	const FTexture2DRHIRef& ReadbackTexture = ReadbackTextures[RenderIndex];

	uint8* StageBuffer;
	int32 Width, Height;
	RHICmdList.MapStagingSurface(ReadbackTexture, CopyResolveFence,
	                             reinterpret_cast<void*&>(StageBuffer), Width, Height);

	FDepthPixel* ReadbackBuffer = AlignedReadbackData[RenderIndex].GetData();
	FMemory::Memcpy(ReadbackBuffer, StageBuffer,
	                AlignedReadbackDataSize.X * AlignedReadbackDataSize.Y * sizeof(FDepthPixel));

	RHICmdList.UnmapStagingSurface(ReadbackTexture);
	RHICmdList.Transition(FRHITransitionInfo(ReadbackTexture, ERHIAccess::CPURead, ERHIAccess::ResolveDst));

	RHICmdList.WriteGPUFence(ReadDepthFence);
}
