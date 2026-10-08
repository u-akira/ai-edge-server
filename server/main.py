from __future__ import annotations

import logging
import os
import secrets

from dotenv import load_dotenv
from fastapi import FastAPI, Header, HTTPException
from google import genai
from google.genai import errors as genai_errors
from pydantic import BaseModel, Field


load_dotenv()

logging.basicConfig(level=os.getenv("LOG_LEVEL", "INFO"))
logger = logging.getLogger("ai-edge-server")

GEMINI_API_KEY = os.getenv("GEMINI_API_KEY", "").strip()
GEMINI_MODEL = os.getenv("GEMINI_MODEL", "gemini-3.8-flash").strip() or "gemini-3.8-flash"
DEVICE_SHARED_TOKEN = os.getenv("DEVICE_SHARED_TOKEN", "").strip()

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
