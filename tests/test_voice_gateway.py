from __future__ import annotations

import asyncio
from types import SimpleNamespace

import pytest
from fastapi.testclient import TestClient

from server import main
from server.voice_gateway import VoiceGatewayConfig, VoiceSessionLimiter, live_connect_config


class FakeLiveSession:
    def __init__(self) -> None:
        self.realtime_inputs: list[dict[str, object]] = []
        self.client_contents: list[dict[str, object]] = []
        self._responses: asyncio.Queue[object] = asyncio.Queue()
        self.empty_response = False

    async def __aenter__(self) -> "FakeLiveSession":
        return self

    async def __aexit__(self, *_: object) -> None:
        return None

    async def send_realtime_input(self, **kwargs: object) -> None:
        self.realtime_inputs.append(kwargs)
        if kwargs.get("audio_stream_end"):
            if self.empty_response:
                self._responses.put_nowait(
                    SimpleNamespace(
                        server_content=SimpleNamespace(
                            model_turn=None,
                            turn_complete=True,
                            output_transcription=None,
                            input_transcription=None,
                            interrupted=False,
                        )
                    )
                )
                return
            self._responses.put_nowait(
                SimpleNamespace(
                    server_content=SimpleNamespace(
                        model_turn=SimpleNamespace(
                            parts=[
                                SimpleNamespace(
                                    inline_data=SimpleNamespace(data=b"\x01\x02")
                                )
                            ]
                        ),
                        turn_complete=False,
                        output_transcription=None,
                        input_transcription=None,
                        interrupted=False,
                    )
                )
            )
            self._responses.put_nowait(
                SimpleNamespace(
                    server_content=SimpleNamespace(
                        model_turn=None,
                        turn_complete=True,
                        output_transcription=None,
                        input_transcription=None,
                        interrupted=False,
                    )
                )
            )

    async def send_client_content(self, **kwargs: object) -> None:
        self.client_contents.append(kwargs)

    async def receive(self):
        while True:
            yield await self._responses.get()


class FakeLiveConnector:
    def __init__(self, session: FakeLiveSession) -> None:
        self.session = session
        self.calls: list[dict[str, object]] = []

    def connect(self, **kwargs: object) -> FakeLiveSession:
        self.calls.append(kwargs)
        return self.session


@pytest.fixture()
def client(monkeypatch: pytest.MonkeyPatch) -> TestClient:
    session = FakeLiveSession()
    connector = FakeLiveConnector(session)
    fake_client = SimpleNamespace(
        aio=SimpleNamespace(live=connector),
        _voice_session=session,
        _voice_connector=connector,
    )
    monkeypatch.setattr(main, "DEVICE_SHARED_TOKEN", "test-device-token")
    monkeypatch.setattr(main, "gemini_client", fake_client)
    monkeypatch.setattr(main, "VOICE_SESSION_LIMITER", VoiceSessionLimiter(2))
    monkeypatch.setattr(
        main,
        "VOICE_GATEWAY_CONFIG",
        VoiceGatewayConfig(model="test-live", max_session_seconds=5),
    )
    return TestClient(main.app)


def test_live_config_uses_audio_and_transcription() -> None:
    config = live_connect_config()

    assert config["response_modalities"] == ["AUDIO"]
    assert config["input_audio_transcription"] == {}
    assert config["output_audio_transcription"] == {}


def test_voice_rejects_invalid_auth(client: TestClient) -> None:
    with client.websocket_connect("/ws/voice") as websocket:
        websocket.send_json({"type": "auth", "token": "wrong-token"})
        assert websocket.receive_json() == {
            "type": "error",
            "code": "unauthorized",
            "message": "Invalid device token",
        }


def test_voice_rejects_binary_auth_frame(client: TestClient) -> None:
    with client.websocket_connect("/ws/voice") as websocket:
        websocket.send_bytes(b"not-json")
        assert websocket.receive_json() == {
            "type": "error",
            "code": "unauthorized",
            "message": "Authentication must be a text message",
        }


def test_voice_rejects_missing_server_token(
    client: TestClient,
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    monkeypatch.setattr(main, "DEVICE_SHARED_TOKEN", "")

    with client.websocket_connect("/ws/voice") as websocket:
        websocket.send_json({"type": "auth", "token": "anything"})
        assert websocket.receive_json() == {
            "type": "error",
            "code": "auth_not_configured",
            "message": "DEVICE_SHARED_TOKEN must be configured",
        }


def test_voice_forwards_pcm_and_streams_response(
    client: TestClient,
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    fake_client = main.gemini_client

    with client.websocket_connect("/ws/voice") as websocket:
        websocket.send_json({"type": "auth", "token": "test-device-token"})
        assert websocket.receive_json()["type"] == "session.ready"

        websocket.send_json({"type": "recording.start"})
        input_audio = b"\x00\x00" * 160
        websocket.send_bytes(input_audio)
        websocket.send_json({"type": "recording.stop"})

        assert websocket.receive_json()["type"] == "response.start"
        assert websocket.receive_bytes() == b"\x01\x02"
        assert websocket.receive_json() == {
            "type": "response.end",
            "cancelled": False,
        }

    session = fake_client._voice_session
    assert session.realtime_inputs[0]["audio"].data == input_audio
    assert session.realtime_inputs[0]["audio"].mime_type == "audio/pcm;rate=16000"
    assert session.realtime_inputs[-1] == {"audio_stream_end": True}


def test_voice_completes_empty_live_turn(
    client: TestClient,
) -> None:
    fake_client = main.gemini_client
    fake_client._voice_session.empty_response = True

    with client.websocket_connect("/ws/voice") as websocket:
        websocket.send_json({"type": "auth", "token": "test-device-token"})
        assert websocket.receive_json()["type"] == "session.ready"

        websocket.send_json({"type": "recording.start"})
        websocket.send_json({"type": "recording.stop"})

        assert websocket.receive_json() == {
            "type": "response.start",
            "sample_rate": 24000,
            "bits_per_sample": 16,
            "channels": 1,
        }
        assert websocket.receive_json() == {
            "type": "response.end",
            "cancelled": False,
        }


def test_voice_cancel_is_forwarded_to_live_session(
    client: TestClient,
) -> None:
    fake_client = main.gemini_client

    with client.websocket_connect("/ws/voice") as websocket:
        websocket.send_json({"type": "auth", "token": "test-device-token"})
        assert websocket.receive_json()["type"] == "session.ready"
        websocket.send_json({"type": "response.cancel"})

    assert fake_client._voice_session.client_contents == [
        {"turns": [], "turn_complete": True}
    ]
