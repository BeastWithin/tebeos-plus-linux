"""Minimal SOAP 1.1 document/literal client with WS-Security UsernameToken.

SGK's Medula web services authenticate via a ``wsse:Security/UsernameToken``
header carrying the eczane (pharmacy) code and password in clear text
(``PasswordText``), inside the SOAP envelope — *not* via HTTP Basic auth.

This module builds the envelope by string template (so namespaces are exactly
what the server expects), POSTs it, and parses the body back into plain Python
dicts with namespaces stripped.
"""

from __future__ import annotations

import xml.etree.ElementTree as ET
from typing import Any

import requests

from .config import Config, ConfigError

WS_SECURITY_NS = "http://docs.oasis-open.org/wss/2004/01/oasis-200401-wss-wssecurity-secext-1.0.xsd"
PASSWORD_TEXT_TYPE = (
    "http://docs.oasis-open.org/wss/2004/01/"
    "oasis-200401-wss-username-token-profile-1.0#PasswordText"
)

_ENVELOPE = (
    '<?xml version="1.0" encoding="UTF-8"?>'
    '<soapenv:Envelope'
    ' xmlns:soapenv="http://schemas.xmlsoap.org/soap/envelope/"'
    ' xmlns:ws="{ns}"'
    ' xmlns:wsse="{wsse}"'
    ' xmlns:xsi="http://www.w3.org/2001/XMLSchema-instance">'
    '<soapenv:Header>'
    '<wsse:Security>'
    '<wsse:UsernameToken>'
    '<wsse:Username>{username}</wsse:Username>'
    '<wsse:Password Type="{pwtype}">{password}</wsse:Password>'
    '</wsse:UsernameToken>'
    '</wsse:Security>'
    '</soapenv:Header>'
    '<soapenv:Body>'
    '<ws:{operation}>{body}</ws:{operation}>'
    '</soapenv:Body>'
    '</soapenv:Envelope>'
)


def _escape(value: str) -> str:
    return (
        value.replace("&", "&amp;")
        .replace("<", "&lt;")
        .replace(">", "&gt;")
        .replace('"', "&quot;")
        .replace("'", "&apos;")
    )


class SoapError(RuntimeError):
    """Raised when the transport fails or the server reports a non-0000 code."""


class SoapClient:
    """Speaks to one SOAP endpoint with WS-Security UsernameToken auth."""

    def __init__(self, config: Config, endpoint: str):
        self.config = config
        self.endpoint = endpoint

    def call(
        self,
        operation: str,
        body_xml: str,
        soap_action: str | None = None,
    ) -> dict[str, Any]:
        """Send one document/literal operation; return the parsed response dict.

        ``body_xml`` is the literal XML placed inside ``<ws:{operation}>``.
        ``soap_action`` becomes the SOAPAction HTTP header (bare action name).
        """
        envelope = _ENVELOPE.format(
            ns=self.config.namespace,
            wsse=WS_SECURITY_NS,
            username=_escape(self.config.eczane_kodu),
            password=_escape(self.config.parola),
            pwtype=PASSWORD_TEXT_TYPE,
            operation=operation,
            body=body_xml,
        )

        headers = {
            "Content-Type": "text/xml; charset=UTF-8",
            "SOAPAction": f'"{soap_action or operation}"',
        }

        try:
            resp = requests.post(
                self.endpoint,
                data=envelope.encode("utf-8"),
                headers=headers,
                timeout=self.config.timeout_seconds,
            )
        except requests.RequestException as exc:
            raise SoapError(f"transport error calling {operation}: {exc}") from exc

        # NOTE: SGK returns HTTP 500 for error results (e.g. 9108/9999) but still
        # embeds a parseable SOAP envelope with the real sonucKodu/sonucMesaji.
        # Parse the body first so callers see the actual error code.
        try:
            root = ET.fromstring(resp.content)
        except ET.ParseError as exc:
            raise SoapError(
                f"HTTP {resp.status_code} + unparseable SOAP response: {exc} "
                f"({resp.text[:200]!r})"
            ) from exc

        body_el = _find_child(root, f"{operation}Response")
        if body_el is not None:
            # Document/literal responses wrap the result in a single "...Return"
            # element; unwrap it so callers see sonucKodu/list fields directly.
            children = list(body_el)
            if len(children) == 1:
                return _element_to_dict(children[0])
            # Return the parsed body regardless of HTTP status (SGK sends error
            # codes inside a HTTP 500); callers decide via sonucKodu.
            return _element_to_dict(body_el)

        # SOAP fault (e.g. <soapenv:Fault>) with no *Response element.
        fault = _find_child(root, "Fault")
        if fault is not None:
            raise SoapError(
                f"SOAP fault calling {operation} (HTTP {resp.status_code}): "
                f"{ET.tostring(fault, encoding='unicode')[:500]}"
            )
        raise SoapError(f"no {operation}Response element in response")


def _strip_ns(tag: str) -> str:
    return tag.rsplit("}", 1)[-1] if "}" in tag else tag


def _find_child(element: ET.Element, local_name: str) -> ET.Element | None:
    for child in element.iter():
        if _strip_ns(child.tag) == local_name:
            return child
    return None


def _element_to_dict(element: ET.Element) -> Any:
    """Convert an XML element subtree into dicts/lists/scalars.

    Repeating child elements (same local name) collapse into a list.
    """
    children: dict[str, list[Any]] = {}
    for child in element:
        name = _strip_ns(child.tag)
        children.setdefault(name, []).append(_element_to_dict(child))

    if not children:
        text = (element.text or "").strip()
        return text

    result: dict[str, Any] = {}
    for name, values in children.items():
        result[name] = values[0] if len(values) == 1 else values
    return result
