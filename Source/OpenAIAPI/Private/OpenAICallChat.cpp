// Copyright Kellan Mythen 2023. All rights Reserved.

#include "OpenAICallChat.h"
#include "OpenAIUtils.h"
#include "Http.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "OpenAIParser.h"

UOpenAICallChat::UOpenAICallChat()
{
}

UOpenAICallChat::~UOpenAICallChat()
{
}

UOpenAICallChat* UOpenAICallChat::OpenAICallChat(FChatSettings ChatSettingsInput)
{
	UOpenAICallChat* BPNode = NewObject<UOpenAICallChat>();
	BPNode->ChatSettings = ChatSettingsInput;
	return BPNode;
}

void UOpenAICallChat::Activate()
{
	FString ApiKey;
	if (UOpenAIUtils::GetUseApiKeyFromEnvironmentVars())
		ApiKey = UOpenAIUtils::GetEnvironmentVariable(TEXT("OPENAI_API_KEY"));
	else
		ApiKey = UOpenAIUtils::GetApiKey();
	
	// checking parameters are valid
	if (ApiKey.IsEmpty())
	{
		Finished.Broadcast({}, TEXT("Api key is not set"), false);
	}	else
	{
	
		TSharedRef<IHttpRequest, ESPMode::ThreadSafe> HttpRequest = FHttpModule::Get().CreateRequest();

		FString ApiModelName;
		switch (ChatSettings.model)
		{
		case EOAChatEngineType::GPT_3_5_TURBO:
			ApiModelName = "gpt-3.5-turbo";
			break;
		case EOAChatEngineType::GPT_4:
			ApiModelName = "gpt-4";
			break;
		case EOAChatEngineType::GPT_4_32k:
			ApiModelName = "gpt-4-32k";
			break;
		case EOAChatEngineType::GPT_4_TURBO:
			ApiModelName = "gpt-4-0125-preview";
			break;
		case EOAChatEngineType::CUSTOM:
			ApiModelName = ChatSettings.customModelName;
			break;
		}
		
		//TODO: add aditional params to match the ones listed in the curl response in: https://platform.openai.com/docs/api-reference/making-requests
	
		// convert parameters to strings
		FString TempHeader = "Bearer ";
		TempHeader += ApiKey;

		// set headers
		FString Url = UOpenAIUtils::GetApiURL();
		HttpRequest->SetURL(Url);
		HttpRequest->SetHeader(TEXT("Content-Type"), TEXT("application/json"));
		HttpRequest->SetHeader(TEXT("Authorization"), TempHeader);

		//build payload
		TSharedPtr<FJsonObject> PayloadObject = MakeShareable(new FJsonObject());
		PayloadObject->SetStringField(TEXT("model"), ApiModelName);
		PayloadObject->SetNumberField(TEXT("max_tokens"), ChatSettings.maxTokens);
		PayloadObject->SetBoolField(TEXT("stream"), ChatSettings.stream);

		
		// convert role enum to model string
		if (!(ChatSettings.messages.Num() == 0))
		{
			TArray<TSharedPtr<FJsonValue>> Messages;
			FString role;
			for (int i = 0; i < ChatSettings.messages.Num(); i++)
			{
				TSharedPtr<FJsonObject> Message = MakeShareable(new FJsonObject());
				switch (ChatSettings.messages[i].role)
				{
				case EOAChatRole::USER:
					role = "user";
					break;
				case EOAChatRole::ASSISTANT:
					role = "assistant";
					break;
				case EOAChatRole::SYSTEM:
					role = "system";
					break;
				}
				Message->SetStringField(TEXT("role"), role);
				Message->SetStringField(TEXT("content"), ChatSettings.messages[i].content);
				Messages.Add(MakeShareable(new FJsonValueObject(Message)));
			}
			PayloadObject->SetArrayField(TEXT("messages"), Messages);
		}

		// convert payload to string
		FString Payload;
		TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Payload);
		FJsonSerializer::Serialize(PayloadObject.ToSharedRef(), Writer);

		// commit request
		HttpRequest->SetVerb(TEXT("POST"));
		HttpRequest->SetContentAsString(Payload);

		if (HttpRequest->ProcessRequest())
		{
			HttpRequest->OnProcessRequestComplete().BindUObject(this, &UOpenAICallChat::OnResponse);

			if (ChatSettings.stream)
			{
				HttpRequest->OnRequestProgress().BindLambda([this](FHttpRequestPtr HttpRequest, int32 BytesSend, int32 InBytesReceived)
				{
					FHttpResponsePtr HttpResponse = HttpRequest->GetResponse();

					if (HttpResponse.IsValid())
					{
						FString Content = *HttpResponse->GetContentAsString();

						//NB: this has some performance implications because it processes all chunks for every delta received.
						//Todo: process only last string...
						TArray<TSharedPtr<FJsonObject>> JsonChunks = ProcessStreamChunkString(Content);

						if (!JsonChunks.IsEmpty() && Streaming.IsBound())
						{
							auto Chunk = JsonChunks.Last();

							FChatCompletion Completion = MessageFromJsonChunk(Chunk);

							bool bDidFinish = !Completion.finishReason.IsEmpty();
							

							if (bDidFinish)
							{
								//Loop over every chunk and re-assemble the whole message for final broadcast
								Completion.message.content = FullMessageFromJsonChunks(JsonChunks);

								Finished.Broadcast(Completion, "", true);
							}
							else
							{
								Streaming.Broadcast(Completion, "", true);
							}
						}
					}
				});
			}
		}
		else
		{
			Finished.Broadcast({}, ("Error sending request"), false);
		}
	}
}

void UOpenAICallChat::OnResponse(FHttpRequestPtr Request, FHttpResponsePtr Response, bool WasSuccessful)
{
	// print response as debug message
	if (!WasSuccessful)
	{
		UE_LOG(LogTemp, Warning, TEXT("Error processing request. \n%s \n%s"), *Response->GetContentAsString(), *Response->GetURL());
		if (Finished.IsBound())
		{
			Finished.Broadcast({}, *Response->GetContentAsString(), false);
		}

		return;
	}

	TSharedPtr<FJsonObject> ResponseObject;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Response->GetContentAsString());
	if (FJsonSerializer::Deserialize(Reader, ResponseObject))
	{
		bool Err = ResponseObject->HasField("error");

		if (Err)
		{
			UE_LOG(LogTemp, Warning, TEXT("%s"), *Response->GetContentAsString());
			Finished.Broadcast({}, TEXT("Api error"), false);
			return;
		}

		OpenAIParser parser(ChatSettings);
		FChatCompletion Out = parser.ParseChatCompletion(*ResponseObject);

		Finished.Broadcast(Out, "", true);
	}
}

TArray<TSharedPtr<FJsonObject>> UOpenAICallChat::ProcessStreamChunkString(const FString& Chunk)
{
	TArray<TSharedPtr<FJsonObject>> JsonChunks;

	TArray<FString> Lines;
	Chunk.ParseIntoArrayLines(Lines);

	for (const FString& Line : Lines)
	{
		//ignore pings
		if (!Line.StartsWith(TEXT("data:")))
		{
			continue;
		}
		const FString& Parsed = Line.Replace(TEXT("data: "), TEXT("")).TrimStartAndEnd();

		TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Parsed);

		TSharedPtr<FJsonObject> Object;

		// Deserialize the JSON string into the JSON object
		FJsonSerializer::Deserialize(Reader, Object);		

		// Log the processed string
		JsonChunks.Add(Object);
	}

	return JsonChunks;
}

TSharedPtr<FJsonObject> UOpenAICallChat::ProcessLastChunkStringFromStream(const FString& Chunk)
{
	TSharedPtr<FJsonObject> DeltaJson;

	TArray<FString> Lines;
	Chunk.ParseIntoArrayLines(Lines);

	if (Lines.Num() > 0)
	{
		const FString& Line = Lines.Last();

		if (!Line.StartsWith(TEXT("data:")))
		{
			DeltaJson;
		}
		const FString& Parsed = Line.Replace(TEXT("data: "), TEXT("")).TrimStartAndEnd();

		TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Parsed);

		// Deserialize the JSON string into the JSON object
		FJsonSerializer::Deserialize(Reader, DeltaJson);
	}

	return DeltaJson;
}

FChatCompletion UOpenAICallChat::MessageFromJsonChunk(TSharedPtr<FJsonObject> Chunk)
{
	FChatCompletion Completion;

	if (!Chunk) 
	{
		return Completion;
	}

	auto Choices = Chunk->GetArrayField(TEXT("choices"));

	if (Choices.Num() > 0)
	{
		FChatLog ChatDelta;
		TSharedPtr<FJsonObject> Choice = Choices[0]->AsObject();

		TSharedPtr<FJsonObject> Delta = Choice->GetObjectField(TEXT("delta"));
		FString FinishReason;
		bool bDidFinish = Choice->TryGetStringField(TEXT("finish_reason"), FinishReason);

		FString RoleString;
		bool bDidFindRole = Delta->TryGetStringField(TEXT("role"), RoleString);

		if (!bDidFindRole)
		{
			RoleString = TEXT("assistant");
		}

		if (RoleString.Equals(TEXT("assistant")))
		{
			ChatDelta.role = EOAChatRole::ASSISTANT;
		}
		else if (RoleString.Equals(TEXT("user")))
		{
			ChatDelta.role = EOAChatRole::USER;
		}
		else
		{
			ChatDelta.role = EOAChatRole::SYSTEM;
		}
		ChatDelta.content = Delta->GetStringField(TEXT("content"));

		Completion.message = ChatDelta;
		Completion.finishReason = FinishReason;

	}
	return Completion;
}

FString UOpenAICallChat::FullMessageFromJsonChunks(TArray<TSharedPtr<FJsonObject>> JsonChunks)
{
	FString FullMessage;

	for (auto Chunk : JsonChunks)
	{
		FullMessage += MessageFromJsonChunk(Chunk).message.content;
	}
	return FullMessage;
}

