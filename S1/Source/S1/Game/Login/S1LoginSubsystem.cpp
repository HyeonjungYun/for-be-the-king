// Fill out your copyright notice in the Description page of Project Settings.

#include "Game/Login/S1LoginSubsystem.h"
#include "Game/Login/S1LoginLogic.h"
#include "Game/UI/S1LoginWidget.h"
#include "Game/UI/S1ItemUIStyle.h"
#include "S1GameInstance.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "HttpModule.h"
#include "Interfaces/IHttpRequest.h"
#include "Interfaces/IHttpResponse.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

using S1ItemUIStyle::Text;

namespace
{
	/** Above S3 (20) and the dungeon item HUD (10) — nothing else is up before login anyway. */
	constexpr int32 LoginZOrder = 30;

	FString ErrorCodeOf(const TSharedPtr<FJsonObject>& Json)
	{
		FString Code;
		if (Json.IsValid())
			Json->TryGetStringField(TEXT("error"), Code);
		return Code;
	}
}

US1LoginSubsystem* US1LoginSubsystem::Get()
{
	if (GWorld == nullptr)
		return nullptr;

	UGameInstance* GameInstance = GWorld->GetGameInstance();
	return GameInstance != nullptr ? GameInstance->GetSubsystem<US1LoginSubsystem>() : nullptr;
}

void US1LoginSubsystem::Deinitialize()
{
	if (UWorld* World = LoginWorld.Get())
		World->OnWorldBeginPlay.Remove(WorldBeginPlayHandle);

	Widget = nullptr;
	Super::Deinitialize();
}

bool US1LoginSubsystem::HasDevToken()
{
	FString Value;
	if (FParse::Value(FCommandLine::Get(), TEXT("token="), Value) && S1LoginLogic::LooksLikeToken(Value))
		return true;

#if WITH_EDITOR
	// Editor binaries only (PIE and UnrealEditor.exe -game). The seeded accounts — dev_1, bot_N — have no
	// password, so without this the editor could not log in at all. A packaged build ignores the ini value
	// and always shows the login screen (account-login-mvp Core Rule 1).
	if (GConfig->GetString(TEXT("/Script/S1.S1GameInstance"), TEXT("DevLoginToken"), Value, GGameIni)
		&& S1LoginLogic::LooksLikeToken(Value))
		return true;
#endif

	return false;
}

bool US1LoginSubsystem::BeginLogin(UWorld* World)
{
	// Both paths connect to the configured address — the dev one through ConnectToGameServer()
	if (US1GameInstance* GameInstance = Cast<US1GameInstance>(GetGameInstance()))
	{
		GameInstance->IpAddress = GameServerHost;
		GameInstance->Port = static_cast<int16>(GameServerPort);
	}

	if (HasDevToken())
	{
		UE_LOG(LogTemp, Log, TEXT("[LOGIN] dev token - connecting without the login screen"));
		return true;
	}

	bActive = true;
	LoginWorld = World;

	// 🔴 Not now. AS1MyPlayer::SetupLocalController sets FInputModeGameAndUI in its own BeginPlay, and which
	//    BeginPlay runs first is not defined — shown now, the screen could lose its UI-only mode with WASD live
	//    behind it. UWorld::BeginPlay broadcasts OnWorldBeginPlay after GameMode->StartPlay() has begun every
	//    actor (World.cpp) — ADR-0011 Decision 5, unreal-specialist B1.
	if (World != nullptr)
		WorldBeginPlayHandle = World->OnWorldBeginPlay.AddUObject(this, &US1LoginSubsystem::ShowScreen);

	UE_LOG(LogTemp, Log, TEXT("[LOGIN] login screen - auth=%s game=%s:%d"), *AuthServerUrl, *GameServerHost, GameServerPort);
	return false;
}

// ── Requests ─────────────────────────────────────────────────────────────

void US1LoginSubsystem::SubmitCredentials(const FString& Username, const FString& Password, bool bRegister)
{
	if (bBusy || Page != ES1LoginPage::Credentials)
		return;

	if (Username.IsEmpty() || Password.IsEmpty())
	{
		Fail(Text(TEXT("login.error.empty")));
		return;
	}

	bBusy = true;
	SetPage(ES1LoginPage::Credentials, Text(TEXT("login.working")));

	if (bRegister == false)
	{
		RequestLogin(Username, Password);
		return;
	}

	TSharedPtr<FJsonObject> Body = MakeShared<FJsonObject>();
	Body->SetStringField(TEXT("username"), Username);
	Body->SetStringField(TEXT("password"), Password);

	// 가입은 성공하면 같은 입력으로 바로 로그인한다 (Core Rule 2)
	SendJson(TEXT("POST"), TEXT("/auth/register"), Body, [this, Username, Password](int32 Code, const TSharedPtr<FJsonObject>& Json)
		{
			if (Code != 201)
			{
				Fail(Code, Json);
				return;
			}

			UE_LOG(LogTemp, Log, TEXT("[LOGIN] registered"));
			RequestLogin(Username, Password);
		});
}

void US1LoginSubsystem::RequestLogin(const FString& Username, const FString& Password)
{
	TSharedPtr<FJsonObject> Body = MakeShared<FJsonObject>();
	Body->SetStringField(TEXT("username"), Username);
	Body->SetStringField(TEXT("password"), Password);

	Token.Reset();
	SendJson(TEXT("POST"), TEXT("/auth/login"), Body, [this](int32 Code, const TSharedPtr<FJsonObject>& Json)
		{
			FString NewToken;
			if (Code != 200 || Json.IsValid() == false || Json->TryGetStringField(TEXT("token"), NewToken) == false
				|| S1LoginLogic::LooksLikeToken(NewToken) == false)
			{
				Fail(Code, Json);
				return;
			}

			// 🔴 The token is a password for the next 60 minutes - never logged (ADR-0011 R-6)
			Token = NewToken;
			UE_LOG(LogTemp, Log, TEXT("[LOGIN] auth ok"));
			RequestCharacters();
		});
}

void US1LoginSubsystem::RequestCharacters()
{
	SendJson(TEXT("GET"), TEXT("/auth/characters"), nullptr, [this](int32 Code, const TSharedPtr<FJsonObject>& Json)
		{
			const TArray<TSharedPtr<FJsonValue>>* Characters = nullptr;
			if (Code != 200 || Json.IsValid() == false || Json->TryGetArrayField(TEXT("characters"), Characters) == false)
			{
				Fail(Code, Json);
				return;
			}

			UE_LOG(LogTemp, Log, TEXT("[LOGIN] characters=%d"), Characters->Num());

			if (Characters->Num() == 0)
			{
				bBusy = false;
				SetPage(ES1LoginPage::CharacterName, FText::GetEmpty());
				if (Widget != nullptr)
					Widget->FocusFirstField();
				return;
			}

			ConnectNextTick();
		});
}

void US1LoginSubsystem::SubmitCharacterName(const FString& Name)
{
	if (bBusy || Page != ES1LoginPage::CharacterName)
		return;

	bBusy = true;
	SetPage(ES1LoginPage::CharacterName, Text(TEXT("login.working")));

	TSharedPtr<FJsonObject> Body = MakeShared<FJsonObject>();
	Body->SetStringField(TEXT("name"), Name);

	// The name rule lives in the auth server only (ADR-0011 Decision 3) - this side just shows its answer
	SendJson(TEXT("POST"), TEXT("/auth/characters"), Body, [this](int32 Code, const TSharedPtr<FJsonObject>& Json)
		{
			// character_limit: another client of this account made one in the meantime - there is one now, use it
			if (Code == 201 || (Code == 409 && ErrorCodeOf(Json) == TEXT("character_limit")))
			{
				UE_LOG(LogTemp, Log, TEXT("[LOGIN] character ready"));
				ConnectNextTick();
				return;
			}

			Fail(Code, Json);
		});
}

void US1LoginSubsystem::SendJson(const FString& Verb, const FString& Path, const TSharedPtr<FJsonObject>& Body, FJsonDone Done)
{
	TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Request = FHttpModule::Get().CreateRequest();
	Request->SetURL(AuthServerUrl + Path);
	Request->SetVerb(Verb);
	Request->SetTimeout(RequestTimeoutSeconds);

	if (Token.IsEmpty() == false)
		Request->SetHeader(TEXT("Authorization"), TEXT("Bearer ") + Token);

	if (Body.IsValid())
	{
		FString Content;
		TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Content);
		FJsonSerializer::Serialize(Body.ToSharedRef(), Writer);

		// SetContentAsString sends UTF-8 - Korean names arrive intact (AuthServer test: round trip)
		Request->SetHeader(TEXT("Content-Type"), TEXT("application/json; charset=utf-8"));
		Request->SetContentAsString(Content);
	}

	// Game-thread completion (the default delegate policy) - the UI may be touched here. The callback can come
	// before ProcessRequest returns when the request fails its pre-checks, so nothing after that call matters.
	TWeakObjectPtr<US1LoginSubsystem> WeakThis(this);
	Request->OnProcessRequestComplete().BindLambda(
		[WeakThis, Done = MoveTemp(Done)](FHttpRequestPtr, FHttpResponsePtr Response, bool bConnected)
		{
			if (WeakThis.IsValid() == false)
				return;

			const int32 Code = (bConnected && Response.IsValid()) ? Response->GetResponseCode() : 0;

			TSharedPtr<FJsonObject> Json;
			if (Code != 0)
			{
				TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Response->GetContentAsString());
				FJsonSerializer::Deserialize(Reader, Json);
			}

			Done(Code, Json);
		});

	Request->ProcessRequest();
}

// ── Game server ──────────────────────────────────────────────────────────

void US1LoginSubsystem::ConnectNextTick()
{
	SetPage(ES1LoginPage::Connecting, Text(TEXT("login.connecting")));

	// ConnectToGameServerWithToken blocks on the socket connect. One frame later, so "접속 중…" is on
	// screen while it does instead of a frozen page with the old text (unreal-specialist note 3)
	if (UGameInstance* GameInstance = GetGameInstance())
		GameInstance->GetTimerManager().SetTimerForNextTick(this, &US1LoginSubsystem::Connect);
}

void US1LoginSubsystem::Connect()
{
	US1GameInstance* GameInstance = Cast<US1GameInstance>(GetGameInstance());
	if (GameInstance == nullptr || GameInstance->ConnectToGameServerWithToken(Token) == false)
	{
		UE_LOG(LogTemp, Warning, TEXT("[LOGIN] game server unreachable - %s:%d"), *GameServerHost, GameServerPort);
		Fail(Text(TEXT("login.error.game_unreachable")));
		return;
	}

	// Stay on "접속 중…" - S_LOGIN decides (HandleGameLoginSucceeded / HandleGameLoginFailed)
	UE_LOG(LogTemp, Log, TEXT("[LOGIN] connected - waiting for S_LOGIN"));
}

void US1LoginSubsystem::HandleGameLoginSucceeded()
{
	if (bActive == false)
		return;

	bBusy = false;
	Token.Reset();		// used up - the game server keeps the session now
	HideScreen();
	UE_LOG(LogTemp, Log, TEXT("[LOGIN] in - base camp"));
}

void US1LoginSubsystem::HandleGameLoginFailed()
{
	// Dev logins have no screen to go back to - ClientPacketHandler has already logged it
	if (bActive == false)
		return;

	// Close the connection that failed - the next attempt opens a new one with a new token.
	// 🔴 Not here: this runs inside PacketSession::HandleRecvPackets' loop, which holds the session only per packet.
	//    Dropping it now frees the object the loop goes on to read - the client crashed at PacketSession.cpp:27
	//    (2026-10-09, second client refused as "already online"). Next tick, outside the loop
	if (UGameInstance* GameInstance = GetGameInstance())
		GameInstance->GetTimerManager().SetTimerForNextTick(this, &US1LoginSubsystem::DisconnectGame);

	Token.Reset();
	Fail(Text(TEXT("login.error.game_server")));
	ShowScreen();
}

void US1LoginSubsystem::DisconnectGame()
{
	if (US1GameInstance* GameInstance = Cast<US1GameInstance>(GetGameInstance()))
		GameInstance->DisconnectFromGameServer();
}

// ── Screen ───────────────────────────────────────────────────────────────

void US1LoginSubsystem::ShowScreen()
{
	if (UWorld* World = LoginWorld.Get())
		World->OnWorldBeginPlay.Remove(WorldBeginPlayHandle);

	UWorld* World = LoginWorld.Get();
	APlayerController* PC = World != nullptr ? World->GetFirstPlayerController() : nullptr;
	if (PC == nullptr)
	{
		UE_LOG(LogTemp, Error, TEXT("[LOGIN] no player controller - cannot show the login screen"));
		return;
	}

	if (Widget == nullptr)
		Widget = CreateWidget<US1LoginWidget>(PC, US1LoginWidget::StaticClass());

	if (Widget == nullptr)
		return;

	if (Widget->IsInViewport() == false)
		Widget->AddToViewport(LoginZOrder);

	// The whole screen, UI only - the pawn behind it must not walk while the player types a password
	PC->bShowMouseCursor = true;
	PC->SetInputMode(FInputModeUIOnly().SetLockMouseToViewportBehavior(EMouseLockMode::DoNotLock));
	Widget->FocusFirstField();
}

void US1LoginSubsystem::HideScreen()
{
	if (Widget != nullptr && Widget->IsInViewport())
		Widget->RemoveFromParent();
}

void US1LoginSubsystem::SetPage(ES1LoginPage NewPage, const FText& NewMessage)
{
	Page = NewPage;
	Message = NewMessage;
	OnChanged.Broadcast();
}

void US1LoginSubsystem::Fail(int32 HttpCode, const TSharedPtr<FJsonObject>& Json)
{
	const FString Code = ErrorCodeOf(Json);
	UE_LOG(LogTemp, Warning, TEXT("[LOGIN] request failed - http=%d error=%s"), HttpCode, *Code);

	// A token the server no longer takes is gone for every page - start over from L1
	if (S1LoginLogic::IsTokenRejected(HttpCode, Code))
	{
		Token.Reset();
		Page = ES1LoginPage::Credentials;
	}

	const TCHAR* Key = S1LoginLogic::ErrorKey(HttpCode, Code);
	Fail(FCString::Strcmp(Key, TEXT("login.error.unknown")) == 0
		? FText::Format(Text(Key), FText::AsNumber(HttpCode))
		: Text(Key));
}

void US1LoginSubsystem::Fail(const FText& Reason)
{
	bBusy = false;

	// "접속 중…" has nothing to retry on its own page - back to where the player can act
	SetPage(Page == ES1LoginPage::Connecting ? ES1LoginPage::Credentials : Page, Reason);

	// The caret goes where the fix is typed - a fresh password on L1 (the id is usually right), the name on L2
	if (Widget != nullptr && Widget->IsInViewport())
		Widget->ResetForRetry();
}
