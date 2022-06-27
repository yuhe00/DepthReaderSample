#pragma once

#include <CoreMinimal.h>

#include "DepthReader.generated.h"

struct FDepthPixel // Pixel depth structure for readback
{
	float Depth;
	// uint8 Stencil;
	// uint8 Align1;
	// uint8 Align2;
	// uint8 Align3;
};

UCLASS(BlueprintType)
class ADepthReader : public AActor
{
	GENERATED_BODY()

public:
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	float ResolutionFraction = .25f;
	
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	bool bDebugOutput;
	
public:
	UFUNCTION(BlueprintCallable)
	FVector GetWorldSpaceMousePosition();
	
	UFUNCTION(BlueprintCallable)
	float GetDepth(const FIntPoint& MousePos);
	
public:
	ADepthReader();

	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	virtual void Tick(float DeltaSeconds) override;

private:
	float SampleDepth(int32 Col, int32 Row);
	FIntPoint GetBufferPosFromMousePos(const FIntPoint& MousePos) const;
	float ConvertDeviceZToDepth(float DeviceZ) const;
	
	FDelegateHandle UpdateCaptureDepthTextureHandle;
	FPostOpaqueRenderDelegate PostOpaqueRenderDelegate;
	
	void UpdateCaptureDepthTexture();
	
	FCriticalSection CriticalSection;
	int CurrentRenderIndex;
	TArray<FDepthPixel> CurrentAlignedReadbackData;
	FVector2D CachedCoordScale;
	FIntPoint CachedReadbackTextureSize;
	FIntPoint CachedAlignedReadbackDataSize;
	float LastDepth;
	FVector4 InvDeviceZToWorldZTransform;

	// Render Thread
	
	void UpdateCaptureDepthTexture_RenderThread(FPostOpaqueRenderParameters& PostOpaqueRenderParameters);
	void CheckDepthTexture_RenderThread(FRHICommandListImmediate& RHICmdList, const FRHITexture2D* DepthTexture);
	void CopyDepthToResolve_RenderThread(FRHICommandListImmediate& RHICmdList, FRHITexture2D* DepthTexture, const int RenderIndex) const;
	void ReadDepthTexture_RenderThread(FRHICommandListImmediate& RHICmdList, const int RenderIndex);

	FGPUFenceRHIRef CopyResolveFence;
	FGPUFenceRHIRef ReadDepthFence;
	FTexture2DRHIRef IntermediateTextures[2];
	FTexture2DRHIRef ReadbackTextures[2];
	TArray<FDepthPixel> AlignedReadbackData[2];
	bool bInitialized;
	FIntRect EffectiveSize;
	FIntPoint DepthBufferSize;
	FIntPoint ReadbackTextureSize;
	FIntPoint AlignedReadbackDataSize;
};