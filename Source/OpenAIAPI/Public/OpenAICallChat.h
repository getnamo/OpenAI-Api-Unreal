// Copyright Kellan Mythen 2023. All rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintAsyncActionBase.h"
#include "OpenAIDefinitions.h"
#include "HttpModule.h"
#include "OpenAICallChat.generated.h"

DECLARE_DYNAMIC_MULTICAST_DELEGATE_ThreeParams(FOnResponseRecievedPin, const FChatCompletion, Message, const FString&, ErrorMessage, bool, Success);

//DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnStreamChunkRecievedPin, const FChatLog, Delta);


/**
 * 
 */
UCLASS()
class OPENAIAPI_API UOpenAICallChat : public UBlueprintAsyncActionBase
{
public:
	GENERATED_BODY()

public:
	UOpenAICallChat();
	~UOpenAICallChat();

	FChatSettings ChatSettings;

	UPROPERTY(BlueprintAssignable, Category = "OpenAI")
	FOnResponseRecievedPin Finished;

	UPROPERTY(BlueprintAssignable, Category = "OpenAI")
	FOnResponseRecievedPin Streaming;

private:
	FString TokenBuffer; // Buffer to accumulate tokens

	UFUNCTION(BlueprintCallable, meta = (BlueprintInternalUseOnly = "true"), Category = "OpenAI")
	static UOpenAICallChat* OpenAICallChat(FChatSettings ChatSettings);

	virtual void Activate() override;
	void OnResponse(FHttpRequestPtr Request, FHttpResponsePtr Response, bool WasSuccessful);

	TArray<TSharedPtr<FJsonObject>> ProcessStreamChunkString(const FString& Chunk);
	TSharedPtr<FJsonObject> ProcessLastChunkStringFromStream(const FString& Chunk);
	FChatCompletion MessageFromJsonChunk(TSharedPtr<FJsonObject> Chunk);
	FString FullMessageFromJsonChunks(TArray<TSharedPtr<FJsonObject>> JsonChunks);
};