"""Pydantic schemas. T04 仅 HealthResponse；write/recall/by_tag 留 T08。"""

from pydantic import BaseModel


class HealthResponse(BaseModel):
    status: str
    neo4j: bool
    embedding: bool
    version: str
