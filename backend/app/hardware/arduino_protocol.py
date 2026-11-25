import json

class ArduinoProtocol:
    """
    Message protocol specification for Arduino Uno R3 communication.
    
    Supports both newline-delimited JSON commands/events and legacy text tokens.
    
    Host -> Arduino (JSON):
      {"type":"REQUEST","id":"<req_id>","agent":"<agent_name>","action":"<action>","risk":"HIGH"}
      {"type":"STATUS","state":"IDLE"|"BLOCKED"|"APPROVED"}
      {"type":"RESET"}
    
    Arduino -> Host (JSON):
      {"type":"HEARTBEAT","status":"ARMED"|"HALTED"}
      {"type":"DECISION","id":"<req_id>","decision":"APPROVE"|"REJECT"}
      {"type":"EMERGENCY_STOP","reason":"HARDWARE_KILL_SWITCH"}
    """

    @staticmethod
    def format_json_request(approval_id: str, agent: str, action: str, risk: str = "HIGH") -> str:
        clean_action = action.split("/")[-1][:20]
        return json.dumps({
            "type": "REQUEST",
            "id": approval_id[:14],
            "agent": agent[:14],
            "action": clean_action,
            "risk": risk[:8]
        }) + "\n"

    @staticmethod
    def format_json_status(state: str) -> str:
        return json.dumps({"type": "STATUS", "state": state.upper()}) + "\n"

    @staticmethod
    def format_json_reset() -> str:
        return json.dumps({"type": "RESET"}) + "\n"

    @staticmethod
    def format_status(status_text: str) -> str:
        return f"STATUS:{status_text[:20]}\n"

    @staticmethod
    def format_pending(approval_id: str, operation: str, target: str, reason: str = "") -> str:
        clean_op = operation[:8].upper()
        clean_target = target.split("/")[-1][:12]
        clean_why = reason[:16].upper() if reason else "REVIEW_REQ"
        # Also sends newline-delimited JSON request format for JSON firmware
        return json.dumps({
            "type": "REQUEST",
            "id": approval_id[:14],
            "agent": clean_op,
            "action": clean_target,
            "risk": clean_why[:8]
        }) + "\n"

    @staticmethod
    def format_decision(approval_id: str, decision: str) -> str:
        return f"DECISION:{approval_id}:{decision.upper()}\n"

    @staticmethod
    def format_veto(reason: str, target: str) -> str:
        clean_reason = reason[:14].strip().upper()
        clean_target = target.split("/")[-1][:14].strip()
        return f"VETO:{clean_reason}:{clean_target}\n"

    @staticmethod
    def format_demo(title: str, step: str) -> str:
        clean_title = title[:14].strip().upper()
        clean_step = step[:14].strip().upper()
        return f"DEMO:{clean_title}:{clean_step}\n"

    @staticmethod
    def format_ping(nonce: str) -> str:
        return f"PING:{nonce}\n"

    @staticmethod
    def format_custom_message(line1: str, line2: str = "", line3: str = "") -> str:
        clean_l1 = line1[:20]
        clean_l2 = line2[:20] if line2 else ""
        clean_l3 = line3[:20] if line3 else ""
        return f"MSG:{clean_l1}:{clean_l2}:{clean_l3}\n"

    @staticmethod
    def parse_message(raw_line: str):
        line = raw_line.strip()
        if not line:
            return None, None

        # 1. Newline-delimited JSON messages
        if line.startswith("{"):
            try:
                data = json.loads(line)
                mtype = data.get("type", "").upper()
                if mtype == "HEARTBEAT":
                    return "HEARTBEAT", {"status": data.get("status", "ARMED")}
                elif mtype == "EMERGENCY_STOP":
                    return "ESTOP", {"reason": data.get("reason", "HARDWARE_KILL_SWITCH")}
                elif mtype == "DECISION":
                    decision = data.get("decision", "").upper()
                    if decision == "APPROVE":
                        return "APPROVE", {"approval_id": data.get("id")}
                    elif decision == "REJECT":
                        return "REJECT", {"approval_id": data.get("id")}
                elif mtype == "PONG":
                    return "PONG", {"nonce": data.get("nonce")}
                return mtype, data
            except Exception:
                pass

        # 2. Legacy token messages
        if line == "READY":
            return "READY", {}
        elif line == "ESTOP":
            return "ESTOP", {}
        elif line == "HEARTBEAT":
            return "HEARTBEAT", {}
        elif line.startswith("I2C:"):
            return "I2C_DIAG", {"raw": line}
        elif line.startswith("OLED:"):
            return "OLED_STATUS", {"raw": line}
        elif line == "VETO:ACK":
            return "VETO_ACK", {}
        elif line == "DEMO:ACK":
            return "DEMO_ACK", {}
        elif line == "MSG:OK":
            return "MSG_ACK", {}
        elif line.startswith("APPROVE:"):
            parts = line.split(":", 1)
            return "APPROVE", {"approval_id": parts[1].strip()}
        elif line.startswith("REJECT:"):
            parts = line.split(":", 1)
            return "REJECT", {"approval_id": parts[1].strip()}
        elif line.startswith("PONG:"):
            parts = line.split(":", 1)
            return "PONG", {"nonce": parts[1].strip()}
        else:
            return "UNKNOWN", {"raw": line}
