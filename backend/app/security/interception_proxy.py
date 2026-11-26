from typing import Dict, Any, Tuple, Optional
from backend.app.models.events import SecurityEvent, SecurityEventCreate, RiskLevel, DecisionType, ExecutionStatus
from backend.app.core.policy_engine import PolicyEngine
from backend.app.core.approval_manager import ApprovalManager
from backend.app.core.execution_manager import ExecutionManager
from backend.app.security.audit_logger import AuditLogger
from backend.app.security.honeytoken_detector import HoneytokenDetector
from backend.app.security.egress_controller import EgressController
from backend.app.security.mcp_integrity import MCPIntegrityVerifier
from backend.app.hardware.heartbeat import HardwareHeartbeat
from backend.app.hardware.arduino_protocol import ArduinoProtocol

class InterceptionProxy:
    """
    Central VETO Interception Layer for AI agent operations.
    Intercepts every tool call, verifies integrity, scans for honeytokens,
    evaluates deterministic policies, coordinates physical and dashboard human approvals,
    executes inside the controlled environment, and logs to the tamper-evident audit chain.
    """

    def __init__(
        self,
        policy_engine: PolicyEngine,
        approval_manager: ApprovalManager,
        execution_manager: ExecutionManager,
        audit_logger: AuditLogger,
        honeytoken_detector: HoneytokenDetector,
        egress_controller: EgressController,
        mcp_verifier: MCPIntegrityVerifier,
        heartbeat: HardwareHeartbeat
    ):
        self.policy_engine = policy_engine
        self.approval_manager = approval_manager
        self.execution_manager = execution_manager
        self.audit_logger = audit_logger
        self.honeytoken_detector = honeytoken_detector
        self.egress_controller = egress_controller
        self.mcp_verifier = mcp_verifier
        self.heartbeat = heartbeat

    def _send_veto_alert(self, reason: str, target: str):
        try:
            if hasattr(self, "approval_manager") and self.approval_manager and hasattr(self.approval_manager, "serial_manager") and self.approval_manager.serial_manager:
                self.approval_manager.serial_manager.send_message(
                    ArduinoProtocol.format_veto(reason, target)
                )
        except Exception:
            pass

    def _send_error_alert(self, reason: str, target: str):
        try:
            if hasattr(self, "approval_manager") and self.approval_manager and hasattr(self.approval_manager, "serial_manager") and self.approval_manager.serial_manager:
                self.approval_manager.serial_manager.send_message(
                    ArduinoProtocol.format_error(reason, target)
                )
        except Exception:
            pass

    def _derive_human_action_rationale(
        self,
        operation: str,
        target_resource: str,
        parameters: Dict[str, Any]
    ) -> Tuple[str, str]:
        if operation in {"delete_file", "rm", "unlink"}:
            return (
                "DESTRUCTIVE FILE REMOVAL: An autonomous AI model requested permanent deletion of a project file. Under Zero-Trust architecture, autonomous agents lack authority to delete workspace assets without verified human consent.",
                "Permanent loss of file contents, broken references, or disruption to dependent software components."
            )
        elif "config" in target_resource or "policies" in target_resource or ".env" in target_resource:
            return (
                "CRITICAL SYSTEM CONFIGURATION MODIFICATION: Proposed changes modify core infrastructure parameters, firewall definitions, or authentication gateways.",
                "Potential security firewall bypass, altered service credentials, or denial of system access."
            )
        elif operation in {"execute_command", "bash", "sh"}:
            cmd = parameters.get("command", "") or target_resource
            return (
                f"ARBITRARY SHELL COMMAND EXECUTION: Agent attempted execution of shell command '{cmd}'. Shell execution introduces unpredictable OS side-effects and arbitrary process spawning.",
                "Unrestricted process execution, local socket binding, or host system reconnaissance."
            )
        elif operation in {"http_request", "network_egress"}:
            return (
                f"UNWHITELISTED NETWORK ACCESS: Outbound request to external destination '{target_resource}'. Requires operator review to confirm destination legitimacy.",
                "External telemetry exfiltration, unmonitored external network sessions, or unauthorized API consumption."
            )
        return (
            f"HIGH-RISK WORKFLOW ACTION: The operation '{operation}' on resource '{target_resource}' exceeded the automated autonomous authorization threshold.",
            "Potential alteration of workspace assets or system behavior."
        )

    async def intercept_operation(
        self,
        agent_id: str,
        session_id: str,
        operation: str,
        target_resource: str,
        parameters: Dict[str, Any]
    ) -> Dict[str, Any]:
        """
        Intercepts tool call prior to execution.
        Returns: {decision, status, reason, event_id, execution_result}
        """

        # 0. Emergency Stop check (Fail-closed)
        if self.heartbeat.emergency_stop_triggered:
            self._send_veto_alert("ESTOP ACTIVE", target_resource)
            event = await self.audit_logger.log_event(SecurityEventCreate(
                agent_id=agent_id,
                session_id=session_id,
                operation=operation,
                target_resource=target_resource,
                risk_level=RiskLevel.CRITICAL,
                decision=DecisionType.VETO,
                policy_reason="BLOCKED: Emergency Stop active. All protected agent operations suspended.",
                execution_outcome=ExecutionStatus.BLOCKED,
                parameters=parameters
            ))
            return {
                "decision": "VETO",
                "status": "BLOCKED",
                "reason": "Emergency Stop active",
                "event_id": event.event_id,
                "execution_result": None
            }

        # 1. MCP Tool Integrity check (if tool call)
        mcp_tool_name = parameters.get("tool_name")
        if operation == "mcp_tool_call" or mcp_tool_name:
            tool_to_check = mcp_tool_name or operation
            is_valid, msg, details = self.mcp_verifier.verify_tool_integrity(tool_to_check)
            if not is_valid:
                # Log threat incident
                self._send_veto_alert("MCP TAMPER", tool_to_check)
                await self.honeytoken_detector.create_threat_incident(
                    threat_type="MCP_INTEGRITY_VIOLATION",
                    description=msg,
                    details=details
                )
                event = await self.audit_logger.log_event(SecurityEventCreate(
                    agent_id=agent_id,
                    session_id=session_id,
                    operation=operation,
                    target_resource=target_resource,
                    risk_level=RiskLevel.CRITICAL,
                    decision=DecisionType.VETO,
                    policy_reason=msg,
                    execution_outcome=ExecutionStatus.BLOCKED,
                    parameters=parameters
                ))
                return {
                    "decision": "VETO",
                    "status": "BLOCKED",
                    "reason": msg,
                    "event_id": event.event_id,
                    "execution_result": None
                }

        # 2. Honeytoken Decoy file access check
        is_decoy, decoy_msg = self.honeytoken_detector.check_resource_access(target_resource)
        if is_decoy:
            self._send_veto_alert("HONEYTOKEN", target_resource)
            await self.honeytoken_detector.create_threat_incident(
                threat_type="HONEYTOKEN_ACCESS_ATTEMPT",
                description=decoy_msg or "Attempted access to decoy file",
                details={"resource": target_resource, "agent": agent_id}
            )
            event = await self.audit_logger.log_event(SecurityEventCreate(
                agent_id=agent_id,
                session_id=session_id,
                operation=operation,
                target_resource=target_resource,
                risk_level=RiskLevel.CRITICAL,
                decision=DecisionType.VETO,
                policy_reason="CRITICAL: Monitored honeytoken decoy file access attempt detected",
                execution_outcome=ExecutionStatus.BLOCKED,
                parameters=parameters
            ))
            return {
                "decision": "VETO",
                "status": "BLOCKED",
                "reason": "CRITICAL: Monitored honeytoken decoy file access attempt detected",
                "event_id": event.event_id,
                "execution_result": None
            }

        # 3. Egress controller check (if network request)
        if operation in {"http_request", "fetch_url", "network_egress"}:
            url = parameters.get("url") or target_resource
            payload = parameters.get("data")
            is_permitted, egress_msg, egress_details = self.egress_controller.inspect_outbound_request(url, payload)
            if not is_permitted:
                self._send_veto_alert("EGRESS BLOCKED", url)
                await self.honeytoken_detector.create_threat_incident(
                    threat_type="EGRESS_EXFILTRATION_BLOCKED",
                    description=egress_msg,
                    details=egress_details
                )
                event = await self.audit_logger.log_event(SecurityEventCreate(
                    agent_id=agent_id,
                    session_id=session_id,
                    operation=operation,
                    target_resource=target_resource,
                    risk_level=RiskLevel.CRITICAL,
                    decision=DecisionType.VETO,
                    policy_reason=egress_msg,
                    execution_outcome=ExecutionStatus.BLOCKED,
                    parameters=parameters
                ))
                return {
                    "decision": "VETO",
                    "status": "BLOCKED",
                    "reason": egress_msg,
                    "event_id": event.event_id,
                    "execution_result": None
                }

        # 4. Deterministic Policy Engine Evaluation
        decision, risk_level, reason, rule_id = self.policy_engine.evaluate(
            operation=operation,
            target_resource=target_resource,
            parameters=parameters
        )

        # 5. Handle Decision:
        if decision == DecisionType.VETO:
            short_why = "BOUNDARY ESCAPE" if "path" in reason.lower() or "boundary" in reason.lower() or "traversal" in reason.lower() else ("BLOCKED CMD" if "command" in reason.lower() else "POLICY VETO")
            self._send_veto_alert(short_why, target_resource)
            event = await self.audit_logger.log_event(SecurityEventCreate(
                agent_id=agent_id,
                session_id=session_id,
                operation=operation,
                target_resource=target_resource,
                risk_level=risk_level,
                decision=DecisionType.VETO,
                policy_reason=reason,
                execution_outcome=ExecutionStatus.BLOCKED,
                parameters=parameters
            ))
            return {
                "decision": "VETO",
                "status": "BLOCKED",
                "reason": reason,
                "rule_id": rule_id,
                "event_id": event.event_id,
                "execution_result": None
            }

        elif decision == DecisionType.REVIEW:
            # Human Review required (Pause execution)
            rationale, impact = self._derive_human_action_rationale(operation, target_resource, parameters)
            approved = await self.approval_manager.request_human_approval(
                session_id=session_id,
                agent_id=agent_id,
                operation=operation,
                target_resource=target_resource,
                parameters=parameters,
                risk_rationale=rationale,
                potential_impact=impact,
                policy_rule=rule_id
            )

            if approved:
                # Execute operation
                exec_result = await self.execution_manager.execute_operation(
                    operation=operation,
                    target_resource=target_resource,
                    parameters=parameters
                )
                if isinstance(exec_result, dict) and exec_result.get("success") is False:
                    self._send_error_alert("EXEC ERROR", target_resource)
                event = await self.audit_logger.log_event(SecurityEventCreate(
                    agent_id=agent_id,
                    session_id=session_id,
                    operation=operation,
                    target_resource=target_resource,
                    risk_level=risk_level,
                    decision=DecisionType.ALLOW,
                    policy_reason=f"Human Approved ({reason})",
                    execution_outcome=ExecutionStatus.EXECUTED,
                    parameters=parameters
                ))
                return {
                    "decision": "ALLOW",
                    "status": "EXECUTED",
                    "reason": f"Authorized by human approval ({reason})",
                    "event_id": event.event_id,
                    "execution_result": exec_result
                }
            else:
                event = await self.audit_logger.log_event(SecurityEventCreate(
                    agent_id=agent_id,
                    session_id=session_id,
                    operation=operation,
                    target_resource=target_resource,
                    risk_level=risk_level,
                    decision=DecisionType.VETO,
                    policy_reason=f"Human Rejected or Timed Out ({reason})",
                    execution_outcome=ExecutionStatus.REJECTED,
                    parameters=parameters
                ))
                return {
                    "decision": "VETO",
                    "status": "REJECTED",
                    "reason": "Operation rejected by human reviewer or approval expired",
                    "event_id": event.event_id,
                    "execution_result": None
                }

        else:  # DecisionType.ALLOW
            exec_result = await self.execution_manager.execute_operation(
                operation=operation,
                target_resource=target_resource,
                parameters=parameters
            )
            if isinstance(exec_result, dict) and exec_result.get("success") is False:
                self._send_error_alert("EXEC ERROR", target_resource)
            event = await self.audit_logger.log_event(SecurityEventCreate(
                agent_id=agent_id,
                session_id=session_id,
                operation=operation,
                target_resource=target_resource,
                risk_level=risk_level,
                decision=DecisionType.ALLOW,
                policy_reason=reason,
                execution_outcome=ExecutionStatus.EXECUTED,
                parameters=parameters
            ))
            return {
                "decision": "ALLOW",
                "status": "EXECUTED",
                "reason": reason,
                "rule_id": rule_id,
                "event_id": event.event_id,
                "execution_result": exec_result
            }
