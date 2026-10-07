from __future__ import annotations

import logging
import os
import secrets

from dotenv import load_dotenv
from fastapi import FastAPI, Header, HTTPException
from openai import APIConnectionError, APIError, APITimeoutError, OpenAI
from pydantic import BaseModel, Field


load_dotenv()

logging.basicConfig(level=os.getenv("LOG_LEVEL", "INFO"))
logger = logging.getLogger("ai-edge-server")

OPENAI_API_KEY = os.getenv("OPENAI_API_KEY", "").strip()
OPENAI_MODEL = os.getenv("OPENAI_MODEL", "gpt-4o-mini").strip() or "gpt-4o-mini"
DEVICE_SHARED_TOKEN = os.getenv("DEVICE_SHARED_TOKEN", "").strip()

openai_client = OpenAI(api_key=OPENAI_API_KEY) if OPENAI_API_KEY else None

app = FastAPI(
    title="AI Edge Server",
    description="A small gateway from edge devices to the OpenAI API.",
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

    if openai_client is None:
        raise HTTPException(
            status_code=503,
            detail="OPENAI_API_KEY is not configured",
        )

    try:
        response = openai_client.responses.create(
            model=OPENAI_MODEL,
            input=request.message,
        )
    except (APIConnectionError, APITimeoutError, APIError) as exc:
        logger.exception("OpenAI API request failed")
        raise HTTPException(
            status_code=502,
            detail="OpenAI API request failed",
        ) from exc

    answer = response.output_text.strip()
    if not answer:
        raise HTTPException(
            status_code=502,
            detail="OpenAI returned an empty response",
        )

    return ChatResponse(message=answer)
