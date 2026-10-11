from __future__ import annotations

import logging
import os
import secrets

from dotenv import load_dotenv
from fastapi import FastAPI, Header, HTTPException, WebSocket
from google import genai
from google.genai import errors as genai_errors
from pydantic import BaseModel, Field

from server.voice_gateway import (
    VoiceGatewayConfig,
    VoiceSessionLimiter,
    handle_voice_websocket,
)


load_dotenv()

logging.basicConfig(level=os.getenv("LOG_LEVEL", "INFO"))
logger = logging.getLogger("ai-edge-server")

GEMINI_API_KEY = os.getenv("GEMINI_API_KEY", "").strip()
GEMINI_MODEL = os.getenv("GEMINI_MODEL", "gemini-3.8-flash").strip() or "gemini-3.8-flash"
DEVICE_SHARED_TOKEN = os.getenv("DEVICE_SHARED_TOKEN", "").strip()
GEMINI_LIVE_MODEL = (
    os.getenv("GEMINI_LIVE_MODEL", "gemini-3.8-live").strip()
    or "gemini-3.8-live"
)


def env_int(name: str, default: int) -> int:
    try:
        return max(1, int(os.getenv(name, str(default))))
    except ValueError:
        return default


VOICE_GATEWAY_CONFIG = VoiceGatewayConfig(
    model=GEMINI_LIVE_MODEL,
    max_session_seconds=env_int("VOICE_MAX_SESSION_SECONDS", 600),
    max_audio_bytes=env_int("VOICE_MAX_AUDIO_BYTES", 8_000_000),
    auth_timeout_seconds=env_int("VOICE_AUTH_TIMEOUT_SECONDS", 10),
)
VOICE_SESSION_LIMITER = VoiceSessionLimiter(
    env_int("VOICE_MAX_SESSIONS", 2),
)

gemini_client = genai.Client() if GEMINI_API_KEY else None

app = FastAPI(
    title="AI Edge Server",
    description="A small gateway from edge devices to the Gemini API.",
    version="0.1.0",
)


class ChatRequest(BaseModel):
    message: str = Field(min_length=1, max_length=4000)


class ChatResponse(BaseModel):
    message: str


@app.get("/health")
def health() -> dict[str, str]:
    """Return a lightweight process health response."""

    return {"status": "ok"}


@app.websocket("/ws/voice")
async def voice(websocket: WebSocket) -> None:
    await handle_voice_websocket(
        websocket,
        gemini_client=gemini_client,
        expected_device_token=DEVICE_SHARED_TOKEN,
        config=VOICE_GATEWAY_CONFIG,
        limiter=VOICE_SESSION_LIMITER,
    )


def verify_device_token(x_device_token: str | None) -> None:
    """Require the shared token when DEVICE_SHARED_TOKEN is configured."""

    if not DEVICE_SHARED_TOKEN:
        return

    if not x_device_token or not secrets.compare_digest(
        x_device_token, DEVICE_SHARED_TOKEN
    ):
        raise HTTPException(status_code=401, detail="Invalid device token")


@app.post("/api/chat", response_model=ChatResponse)
def chat(
    request: ChatRequest,
    x_device_token: str | None = Header(default=None),
) -> ChatResponse:
    verify_device_token(x_device_token)

    if gemini_client is None:
        raise HTTPException(
            status_code=503,
            detail="GEMINI_API_KEY is not configured",
        )

    try:
        response = gemini_client.models.generate_content(
            model=GEMINI_MODEL,
            contents=request.message,
        )
    except genai_errors.APIError as exc:
        logger.exception("Gemini API request failed")
        raise HTTPException(
            status_code=502,
            detail="Gemini API request failed",
        ) from exc

    answer = (response.text or "").strip()
    if not answer:
        raise HTTPException(
            status_code=502,
            detail="Gemini returned an empty response",
        )

    return ChatResponse(message=answer)
