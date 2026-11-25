class ArduinoProtocol:
    """
    Message protocol specification for Arduino Uno R3 communication.
    
    Host -> Arduino:
      STATUS:<text>                (e.g., STATUS:SYSTEM NORMAL)
      PENDING:<id>:<op>:<target>   (e.g., PENDING:APR-9812:DELETE:system_config.json)
      DECISION:<id>:<decision>     (e.g., DECISION:APR-9812:APPROVED)
      VETO:<reason>:<target>       (e.g., VETO:HONEYTOKEN:.env.decoy)
      DEMO:<title>:<step>          (e.g., DEMO:SCENARIO 1:HONEYTOKEN EXFIL)
      MSG:<l1>:<l2>:<l3>           (e.g., MSG:TITLE:SUBTITLE:STATUS)
      PING:<nonce>                 (Heartbeat keepalive query)
      ESTOP_RESET                  (Clears emergency-stop state on Arduino)
      SCAN                         (Run I2C diagnostic scan on Arduino bus)
    
    Arduino -> Host:
      READY                        (Sent when Arduino boots or resets)
      APPROVE:<id>                 (Physical APPROVE button pressed for active ID)
      REJECT:<id>                  (Physical REJECT button pressed for active ID)
      ESTOP                        (Physical EMERGENCY-STOP button triggered)
      PONG:<nonce>                 (Heartbeat keepalive response)
      HEARTBEAT                    (Periodic status ping from Arduino)
      VETO:ACK                     (Veto alert received and displayed on OLED)
      DEMO:ACK                     (Demo scenario alert received and displayed on OLED)
      MSG:OK                       (Custom message acknowledged)
      I2C:FOUND:<addr>             (I2C diagnostic scan result)
      OLED:INIT_SUCCESS            (OLED initialized successfully)
    """

    @staticmethod
    def format_status(status_text: str) -> str:
        return f"STATUS:{status_text[:20]}\n"

    @staticmethod
    def format_pending(approval_id: str, operation: str, target: str, reason: str = "") -> str:
        # Arduino OLED line width is limited (approx 16-20 chars)
        clean_op = operation[:8].upper()
        clean_target = target.split("/")[-1][:12]
        clean_why = reason[:16].upper() if reason else "REVIEW_REQ"
        return f"PENDING:{approval_id}:{clean_op}:{clean_target}:{clean_why}\n"

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
