from types import SimpleNamespace

import pytest
from fastapi.testclient import TestClient

from server import main


@pytest.fixture()
def client() -> TestClient:
    return TestClient(main.app)


def test_health_returns_ok(client: TestClient) -> None:
    response = client.get("/health")

    assert response.status_code == 200
    assert response.json() == {"status": "ok"}


@pytest.mark.parametrize(
    "token_header",
    [None, "wrong-token"],
    ids=["missing-token", "invalid-token"],
)
def test_chat_rejects_missing_or_invalid_device_token(
    monkeypatch: pytest.MonkeyPatch,
    client: TestClient,
    token_header: str | None,
) -> None:
    monkeypatch.setattr(main, "DEVICE_SHARED_TOKEN", "test-device-token")

    headers = {} if token_header is None else {"X-Device-Token": token_header}
    response = client.post(
        "/api/chat",
        headers=headers,
        json={"message": "hello"},
    )

    assert response.status_code == 401
    assert response.json() == {"detail": "Invalid device token"}


def test_chat_returns_openai_response_for_valid_device_token(
    monkeypatch: pytest.MonkeyPatch,
    client: TestClient,
) -> None:
    monkeypatch.setattr(main, "DEVICE_SHARED_TOKEN", "test-device-token")
    fake_openai_client = SimpleNamespace(
        responses=SimpleNamespace(
            create=lambda **_: SimpleNamespace(output_text="test answer")
        )
    )
    monkeypatch.setattr(main, "openai_client", fake_openai_client)

    response = client.post(
        "/api/chat",
        headers={"X-Device-Token": "test-device-token"},
        json={"message": "hello"},
    )

    assert response.status_code == 200
    assert response.json() == {"message": "test answer"}


def test_chat_returns_service_unavailable_without_openai_client(
    monkeypatch: pytest.MonkeyPatch,
    client: TestClient,
) -> None:
    monkeypatch.setattr(main, "DEVICE_SHARED_TOKEN", "test-device-token")
    monkeypatch.setattr(main, "openai_client", None)

    response = client.post(
        "/api/chat",
        headers={"X-Device-Token": "test-device-token"},
        json={"message": "hello"},
    )

    assert response.status_code == 503
    assert response.json() == {"detail": "OPENAI_API_KEY is not configured"}


def test_chat_rejects_empty_message(
    monkeypatch: pytest.MonkeyPatch,
    client: TestClient,
) -> None:
    monkeypatch.setattr(main, "DEVICE_SHARED_TOKEN", "test-device-token")

    response = client.post(
        "/api/chat",
        headers={"X-Device-Token": "test-device-token"},
        json={"message": ""},
    )

    assert response.status_code == 422
