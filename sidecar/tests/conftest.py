"""Shared fixtures and wire payload builders for the verification client tests."""

from __future__ import annotations

import copy
from typing import Any

import pytest

TOKEN = "a" * 64
PORT = "48765"


@pytest.fixture
def anyio_backend() -> str:
    """The MCP tools are async, so the anyio plugin needs a backend to run them on."""
    return "asyncio"


def environment(**overrides: str | None) -> dict[str, str]:
    """Builds a complete environment mapping, dropping any key overridden with None."""
    env: dict[str, str | None] = {
        "PLAYERBOT_VERIFICATION_PORT": PORT,
        "PLAYERBOT_VERIFICATION_TOKEN": TOKEN,
    }
    env.update(overrides)
    return {key: value for key, value in env.items() if value is not None}


def completeness(total: int, returned: int, truncated: bool) -> dict[str, Any]:
    return {"totalCount": total, "returnedCount": returned, "truncated": truncated}


def action_attempt(
    sequence: int, name: str, *, success: bool = True, truncated: bool = False
) -> dict[str, Any]:
    return {
        "sequence": sequence,
        "timestampMs": 1_700_000_000_000 + sequence,
        "ageMs": 250,
        "success": success,
        "actionName": name,
        "nameTruncated": truncated,
    }


def latest_attempt(
    sequence: int, name: str, *, success: bool = True, truncated: bool = False
) -> dict[str, Any]:
    """The server wraps the newest attempt with an availability flag."""
    return {"available": True} | action_attempt(sequence, name, success=success, truncated=truncated)


def inspection_payload(**overrides: Any) -> dict[str, Any]:
    """Builds a complete verification inspection exactly as the C++ serializer emits it."""
    payload: dict[str, Any] = {
        "schemaVersion": 4,
        "ok": True,
        "identity": {
            "guid": "0x0000000000000003",
            "name": "Grimtusk",
            "level": 42,
            "raceId": 2,
            "classId": 1,
        },
        "master": {
            "available": True,
            "guid": "0x0000000000000001",
            "name": "Pierre",
            "relationshipValid": True,
        },
        "group": {
            "available": True,
            "guid": "0x0000000000000009",
            "leaderGuid": "0x0000000000000001",
            "members": [{"guid": "0x0000000000000001", "name": "Pierre", "subgroup": 0, "leader": True}],
            "completeness": completeness(1, 1, False),
        },
        "position": {
            "mapId": 571,
            "zoneId": 3537,
            "areaId": 4080,
            "x": 5807.123,
            "y": 604.456,
            "z": 646.789,
            "orientation": 3.142,
            "movementFlags": 1,
            "moving": True,
            "movementState": "moving",
        },
        "transport": {"attached": False, "guid": "", "entry": 0},
        "travel": {
            "available": True,
            "status": "travel",
            "destination": {
                "type": "RpgTravelDestination",
                "title": "Botanist Tyniarrel",
                "distanceYards": 311.18,
            },
            "forced": True,
            "canMove": True,
            "route": {
                "pointCount": 8,
                "nextPathType": "walk",
                "nextEntry": 0,
                "nextPoint": {
                    "available": True,
                    "mapId": 530,
                    "x": 8704.0,
                    "y": -6620.0,
                    "z": 72.5,
                    "distanceYards": 18.9,
                },
            },
            "lastMovement": {
                "point": {
                    "available": True,
                    "mapId": 530,
                    "x": 8703.0,
                    "y": -6639.0,
                    "z": 72.7,
                    "distanceYards": 0.2,
                },
                "ageMs": 5100,
                "delayMs": 5000,
                "priority": "forced",
            },
        },
        "rpgTarget": {
            "available": True,
            "type": "creature",
            "guid": "Creature-0-1-14990-208472",
            "entry": 14990,
            "name": "Defilers Emissary",
            "npcFlags": 1048577,
            "distanceYards": 37.5,
            "moving": True,
        },
        "action": {
            "lastExecutedAction": "melee",
            "latestAttempt": latest_attempt(7, "melee"),
            "attempts": [action_attempt(6, "follow master", success=False), action_attempt(7, "melee")],
            "completeness": completeness(7, 2, True),
        },
        "finance": {"moneyCopper": 123_456_789, "freeTradeskillCopper": 9_000, "freeSpellsCopper": 4_500},
        "career": {
            "status": "valid",
            "version": 3,
            "candidateToken": "blacksmith-mining",
            "primarySkillIds": [164, 186],
            "secondarySkillIds": [129],
            "spendingStyle": "progression",
            "marketEligible": True,
            "engagement": 2,
            "source": "loaded",
        },
        "knownRecipeSpellIds": {"items": [2018, 3100, 3538, 9785], "completeness": completeness(4, 4, False)},
        "economy": {
            "available": True,
            "sequence": 88,
            "phase": "buy_reagent",
            "outcome": "operation",
            "chainPublicId": "chn_0123456789abcdef",
            "operationIdentity": "buy_reagent:2589:20",
            "marketId": 7,
            "itemFamily": "exact_reagent:2589",
            "workOrderSpellId": 3538,
            "remainingQuantity": 20,
            "claimAgeSeconds": 12,
            "blockerCode": "",
            "consecutiveFailures": 0,
            "cooldownSeconds": 60,
            "nextEligibleTime": 1_700_000_500,
            "quarantined": False,
        },
        "equipment": {
            "items": [{"slot": 0, "itemId": 12640, "name": "Lionheart Helm", "count": 1}],
            "completeness": completeness(1, 1, False),
        },
        "inventory": {
            "items": [{"itemId": 2589, "name": "Linen Cloth", "count": 20}],
            "completeness": completeness(1, 1, False),
        },
        "skills": {
            "items": [{"id": 43, "name": "Swords", "value": 210, "maximum": 210}],
            "completeness": completeness(1, 1, False),
        },
        "professions": {
            "items": [{"id": 164, "name": "Blacksmithing", "value": 300, "maximum": 300}],
            "completeness": completeness(1, 1, False),
        },
    }
    payload.update(copy.deepcopy(overrides))
    return payload


def envelope(request_id: int, result: dict[str, Any]) -> dict[str, Any]:
    return {"schemaVersion": 2, "requestId": request_id, "ok": True, "result": result}


def error_envelope(request_id: int, code: str, message: str = "The operation failed.") -> dict[str, Any]:
    return {
        "schemaVersion": 2,
        "requestId": request_id,
        "ok": False,
        "error": {"code": code, "message": message},
    }
