from __future__ import annotations

import asyncio
import base64
import binascii
import json
import logging
import secrets
import time
from dataclasses import dataclass
from typing import Any

from fastapi import WebSocket, WebSocketDisconnect
from google.genai import types


logger = logging.getLogger("ai-edge-server.voice")


@dataclass(frozen=True)
class VoiceGatewayConfig:
    model: str
    max_session_seconds: int = 600
    max_audio_bytes: int = 8_000_000
    auth_timeout_seconds: int = 10


@dataclass
class VoiceTurnState:
    recording_active: bool = False
    audio_bytes: int = 0
    response_active: bool = False
    response_cancelled: bool = False
    response_end_sent: bool = False
    recording_stopped_at: float | None = None
    first_audio_sent_at: float | None = None


class VoiceSessionLimiter:
    """Small async-safe limiter that does not require an event loop at import time."""

    def __init__(self, maximum: int) -> None:
        if maximum < 1:
            raise ValueError("maximum must be at least 1")
        self._maximum = maximum
        self._active = 0
        self._lock: asyncio.Lock | None = None

    async def try_acquire(self) -> bool:
        if self._lock is None:
            self._lock = asyncio.Lock()
        async with self._lock:
            if self._active >= self._maximum:
                return False
            self._active += 1
            return True

    async def release(self) -> None:
        if self._lock is None:
            return
        async with self._lock:
            self._active = max(0, self._active - 1)


def live_connect_config() -> dict[str, Any]:
    """Return the stable part of the Live API configuration."""

    return {
        "response_modalities": ["AUDIO"],
        "system_instruction": (
            "あなたはCoreS3上の音声アシスタントです。"
            "日本語で、自然で簡潔に回答してください。"
        ),
        "input_audio_transcription": {},
        "output_audio_transcription": {},
    }


def _token_matches(token: str | None, expected_token: str) -> bool:
    return bool(expected_token and token) and secrets.compare_digest(token, expected_token)


async def _send_json(websocket: WebSocket, payload: dict[str, Any]) -> None:
    await websocket.send_text(json.dumps(payload, ensure_ascii=False))


async def _send_error(
    websocket: WebSocket,
    code: str,
    message: str,
) -> None:
    await _send_json(
        websocket,
        {"type": "error", "code": code, "message": message},
    )


def _decode_audio_data(data: Any) -> bytes:
    if isinstance(data, bytes):
        return data
    if isinstance(data, bytearray):
        return bytes(data)
    if isinstance(data, str):
        try:
            return base64.b64decode(data, validate=True)
        except (ValueError, binascii.Error) as exc:
            raise ValueError("Invalid base64 audio data") from exc
    return b""


async def _handle_control_message(
    websocket: WebSocket,
    gemini_session: Any,
    state: VoiceTurnState,
    control: dict[str, Any],
) -> None:
    event_type = control.get("type")

    if event_type == "recording.start":
        if state.recording_active:
            await _send_error(websocket, "invalid_state", "Recording is already active")
            return
        state.recording_active = True
        state.audio_bytes = 0
        state.response_cancelled = False
        state.response_end_sent = False
        state.recording_stopped_at = None
        state.first_audio_sent_at = None
        return

    if event_type == "recording.stop":
        if not state.recording_active:
            await _send_error(websocket, "invalid_state", "Recording is not active")
            return
        state.recording_active = False
        state.recording_stopped_at = time.monotonic()
        logger.info("voice recording stopped; sending audio_stream_end")
        # Keep Live API automatic VAD enabled, but finalize immediately on STOP.
        await gemini_session.send_realtime_input(audio_stream_end=True)
        return

    if event_type == "response.cancel":
        state.response_cancelled = True
        await gemini_session.send_client_content(turns=[], turn_complete=True)
        if state.response_active and not state.response_end_sent:
            await _send_json(
                websocket,
                {"type": "response.end", "cancelled": True},
            )
            state.response_end_sent = True
        return

    if event_type == "ping":
        await _send_json(websocket, {"type": "pong"})
        return

    await _send_error(websocket, "unknown_event", "Unsupported control event")


async def _device_to_gemini(
    websocket: WebSocket,
    gemini_session: Any,
    state: VoiceTurnState,
    config: VoiceGatewayConfig,
) -> None:
    while True:
        try:
            message = await websocket.receive()
        except WebSocketDisconnect:
            return

        if message.get("type") == "websocket.disconnect":
            return

        text = message.get("text")
        if text is not None:
            try:
                control = json.loads(text)
            except json.JSONDecodeError:
                await _send_error(websocket, "invalid_json", "Invalid control message")
                continue

            if not isinstance(control, dict):
                await _send_error(websocket, "invalid_message", "Control message must be an object")
                continue
            await _handle_control_message(websocket, gemini_session, state, control)
            continue

        audio = message.get("bytes")
        if audio is None:
            continue
        if not state.recording_active:
            await _send_error(websocket, "invalid_state", "Send recording.start first")
            continue
        if len(audio) == 0 or len(audio) % 2 != 0:
            await _send_error(websocket, "invalid_audio", "Audio must be non-empty 16-bit PCM")
            continue
        if state.audio_bytes + len(audio) > config.max_audio_bytes:
            await _send_error(websocket, "audio_limit", "Audio session limit exceeded")
            return

        state.audio_bytes += len(audio)
        await gemini_session.send_realtime_input(
            audio=types.Blob(data=audio, mime_type="audio/pcm;rate=16000")
        )


async def _gemini_to_device(
    websocket: WebSocket,
    gemini_session: Any,
    state: VoiceTurnState,
) -> None:
    async for response in gemini_session.receive():
        server_content = getattr(response, "server_content", None)
        if server_content is None:
            continue

        if getattr(server_content, "interrupted", False):
            state.response_cancelled = True

        model_turn = getattr(server_content, "model_turn", None)
        if model_turn is not None:
            if not state.response_active:
                await _send_json(
                    websocket,
                    {
                        "type": "response.start",
                        "sample_rate": 24000,
                        "bits_per_sample": 16,
                        "channels": 1,
                    },
                )
                state.response_active = True

            for part in getattr(model_turn, "parts", []) or []:
                inline_data = getattr(part, "inline_data", None)
                if inline_data is not None and not state.response_cancelled:
                    audio = _decode_audio_data(getattr(inline_data, "data", None))
                    if audio:
                        if state.first_audio_sent_at is None:
                            state.first_audio_sent_at = time.monotonic()
                            if state.recording_stopped_at is not None:
                                logger.info(
                                    "voice latency recording_stop_to_first_audio_ms=%d",
                                    int(
                                        (state.first_audio_sent_at - state.recording_stopped_at)
                                        * 1000
                                    ),
                                )
                        await websocket.send_bytes(audio)

                text = getattr(part, "text", None)
                if text:
                    await _send_json(
                        websocket,
                        {"type": "transcript", "direction": "output", "text": text},
                    )

        output_transcription = getattr(server_content, "output_transcription", None)
        if output_transcription is not None:
            text = getattr(output_transcription, "text", None)
            if text:
                await _send_json(
                    websocket,
                    {"type": "transcript", "direction": "output", "text": text},
                )

        input_transcription = getattr(server_content, "input_transcription", None)
        if input_transcription is not None:
            text = getattr(input_transcription, "text", None)
            if text:
                await _send_json(
                    websocket,
                    {"type": "transcript", "direction": "input", "text": text},
                )

        if getattr(server_content, "turn_complete", False):
            if not state.response_active:
                await _send_json(
                    websocket,
                    {
                        "type": "response.start",
                        "sample_rate": 24000,
                        "bits_per_sample": 16,
                        "channels": 1,
                    },
                )
                state.response_active = True
            if state.response_active and not state.response_end_sent:
                await _send_json(
                    websocket,
                    {"type": "response.end", "cancelled": state.response_cancelled},
                )
            state.response_active = False
            state.response_cancelled = False
            state.response_end_sent = False


async def _run_bridge(
    websocket: WebSocket,
    gemini_session: Any,
    config: VoiceGatewayConfig,
) -> None:
    state = VoiceTurnState()
    device_task = asyncio.create_task(
        _device_to_gemini(websocket, gemini_session, state, config)
    )
    gemini_task = asyncio.create_task(_gemini_to_device(websocket, gemini_session, state))
    tasks = {device_task, gemini_task}

    try:
        done, _ = await asyncio.wait(tasks, return_when=asyncio.FIRST_COMPLETED)
        for task in done:
            task.result()
    finally:
        for task in tasks:
            if not task.done():
                task.cancel()
        await asyncio.gather(*tasks, return_exceptions=True)


async def handle_voice_websocket(
    websocket: WebSocket,
    *,
    gemini_client: Any,
    expected_device_token: str,
    config: VoiceGatewayConfig,
    limiter: VoiceSessionLimiter,
) -> None:
    await websocket.accept()

    if not await limiter.try_acquire():
        await _send_error(websocket, "busy", "Maximum voice sessions reached")
        await websocket.close(code=1013, reason="voice session limit")
        return

    try:
        try:
            auth_message_frame = await asyncio.wait_for(
                websocket.receive(), timeout=config.auth_timeout_seconds
            )
        except asyncio.TimeoutError:
            await _send_error(websocket, "unauthorized", "Authentication required")
            await websocket.close(code=1008, reason="authentication required")
            return
        except WebSocketDisconnect:
            return

        if auth_message_frame.get("type") == "websocket.disconnect":
            return
        auth_text = auth_message_frame.get("text")
        if not isinstance(auth_text, str):
            await _send_error(websocket, "unauthorized", "Authentication must be a text message")
            await websocket.close(code=1008, reason="invalid authentication")
            return

        try:
            auth_message = json.loads(auth_text)
        except json.JSONDecodeError:
            await _send_error(websocket, "unauthorized", "Invalid authentication message")
            await websocket.close(code=1008, reason="invalid authentication")
            return

        if not expected_device_token:
            await _send_error(
                websocket,
                "auth_not_configured",
                "DEVICE_SHARED_TOKEN must be configured",
            )
            await websocket.close(code=1011, reason="device authentication is not configured")
            return

        if (
            not isinstance(auth_message, dict)
            or auth_message.get("type") != "auth"
            or not _token_matches(auth_message.get("token"), expected_device_token)
        ):
            await _send_error(websocket, "unauthorized", "Invalid device token")
            await websocket.close(code=1008, reason="invalid device token")
            return

        if gemini_client is None:
            await _send_error(websocket, "gemini_not_configured", "Gemini API is not configured")
            await websocket.close(code=1011, reason="Gemini is not configured")
            return

        try:
            async with gemini_client.aio.live.connect(
                model=config.model,
                config=live_connect_config(),
            ) as gemini_session:
                await _send_json(
                    websocket,
                    {"type": "session.ready", "sample_rate": 16000},
                )
                try:
                    async with asyncio.timeout(config.max_session_seconds):
                        await _run_bridge(websocket, gemini_session, config)
                except TimeoutError:
                    await _send_error(websocket, "session_timeout", "Voice session timed out")
        except Exception:
            logger.exception("Gemini Live voice session failed")
            try:
                await _send_error(websocket, "upstream_error", "Gemini Live session failed")
            except Exception:
                pass
    finally:
        await limiter.release()
        try:
            await websocket.close()
        except Exception:
            pass
