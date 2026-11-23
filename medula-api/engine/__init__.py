"""Medula (SGK) SOAP engine — OS-neutral client for TEBEOS pharmacy integration.

This package wraps SGK's pharmacy web services as clean Python calls so the
integration is independent of the vendor's IE/wine screen-scraping. It only
talks SOAP/WS-Security; no browser, no captcha.

Confirmed-working endpoint: ``eczaneStokIslemleriWS`` (daily stock query).
"""

from .config import Config, ConfigError, load_config
from .drugs import DrugListClient
from .soap import SoapClient, SoapError
from .stock import StockClient

__all__ = [
    "Config",
    "ConfigError",
    "load_config",
    "SoapClient",
    "SoapError",
    "StockClient",
    "DrugListClient",
]
