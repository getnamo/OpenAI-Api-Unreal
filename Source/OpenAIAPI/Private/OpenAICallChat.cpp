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

UOpenAICallChat* UOpenAICallChat::OpenAICallChat(FChatSettings chatSettingsInput)
{
	UOpenAICallChat* BPNode = NewObject<UOpenAICallChat>();
	BPNode->ChatSettings = chatSettingsInput;
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
	
		auto HttpRequest = FHttpModule::Get().CreateRequest();

		FString apiMethod;
		switch (ChatSettings.model)
		{
		case EOAChatEngineType::GPT_3_5_TURBO:
			apiMethod = "gpt-3.5-turbo";
			break;
		case EOAChatEngineType::GPT_4:
			apiMethod = "gpt-4";
			break;
		case EOAChatEngineType::GPT_4_32k:
			apiMethod = "gpt-4-32k";
			break;
			case EOAChatEngineType::GPT_4_TURBO:
				apiMethod = "gpt-4-0125-preview";
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
		PayloadObject->SetStringField(TEXT("model"), apiMethod);
		PayloadObject->SetNumberField(TEXT("max_tokens"), ChatSettings.maxTokens);

		
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

