# ECSE3038 - Tutorial 6 starter: the readings API from Friday's lecture.
# Nothing to write in here today. Today's job is to run it on your VM.

from datetime import datetime, timezone
import secrets
import json

from typing import Literal
from fastapi import FastAPI, HTTPException, Response, Header, Query, status
from fastapi.middleware.cors import CORSMiddleware
from pydantic import BaseModel, Field

import os
from dotenv import load_dotenv
from pymongo import MongoClient, ASCENDING
from pymongo.errors import DuplicateKeyError

import paho.mqtt.publish as publish

load_dotenv()
client = MongoClient(os.getenv("MONGODB_URI"), tz_aware=True)
db = client["tutorial6"]
fans = db["fans"]
readings = db["readings"]
settings = db["settings"]
MQTT_BROKER = "135.232.203.144"
MQTT_NAME = "qwerty2"

fans.create_index([("mac", ASCENDING)], unique=True)

app = FastAPI()
app.add_middleware(
    CORSMiddleware,
    allow_origins=["*"],
    allow_methods=["*"],
    allow_headers=["*"],
)

class Fan(BaseModel):
    mac: str
    room: str | None = None


class Reading(BaseModel):
    mac: str
    humidity: float = Field(ge=1, le=100)

class Settings(BaseModel):
    mode: Literal["auto", "on", "off"]
    temp_limit: float = Field(ge=0, le=100)

#THE FISRT TASKKK  (A1)

@app.get("/fans")
def get_fans():
    return list(fans.find({}, {"_id": 0, "key": 0}))


@app.get("/fans/{mac}")
def get_fan(mac: str):
    fan = fans.find_one({"mac": mac}, {"_id": 0, "key": 0})

    if fan is None:
        raise HTTPException(status_code=404, detail="No such fan was found.")

    return fan


@app.post("/fans", status_code=201)
def create_fan(fan: Fan):
    new_fan = fan.model_dump()
    new_fan["key"] = secrets.token_urlsafe(16)
    try:
        fans.insert_one(new_fan)
    except DuplicateKeyError:
        raise HTTPException(status_code=409, detail=f"Fan with MAC {fan.mac} already exists.")
    new_fan.pop("_id", None)
    return new_fan


#THE SECOND TASK (A2)
@app.post("/readings", status_code=201)
def create_reading(reading: Reading, x_api_key: str | None = Header(default=None, alias="X-API-Key")):
    if not x_api_key:
        raise HTTPException(status_code=401, detail="Missing X-API-Key")
    
    key_owner = fans.find_one({"key": x_api_key})
    if key_owner is None:
        raise HTTPException(status_code=401, detail="Invalid X-API-Key")

    fan = fans.find_one({"mac": reading.mac})
    if fan is None:
        raise HTTPException(status_code=404, detail="No fan with MAC " + reading.mac)

    if key_owner["mac"] != reading.mac:
        raise HTTPException(status_code=403, detail="API key does not belong to this fan")

    new_reading = reading.model_dump()
    new_reading["time"] = datetime.now(timezone.utc)
    readings.insert_one(new_reading)
    new_reading.pop("_id", None)
    return new_reading

#THE SECOND TASK (A3)

@app.get("/readings")
def get_readings(mac: str | None = None, since: datetime | None = None, limit: int = Query(default=10)):
    query = {}
    if mac is not None:
        query["mac"] = mac
    if since is not None:
        query["time"] = {"$gte": since}
    return list(readings.find(query, {"_id": 0, "key": 0}).sort("time", -1).limit(limit))

#THE FOURTH TASK (A4)
@app.put("/fans/{mac}/settings")
def put_settings(mac: str, new: Settings, response: Response):
    if not fans.find_one({"mac": mac}):
        raise HTTPException(status_code=404, detail="Fan not found")

    doc = new.model_dump()
    doc["mac"] = mac
    
    result = settings.replace_one({"mac": mac}, doc, upsert=True)
    if result.upserted_id is not None:
        response.status_code = 201
    else:
        response.status_code = 200
        
    doc.pop("_id", None)
    
    try:
        publish.single(
            f"ecse3038/{MQTT_NAME}/fan/settings",
            json.dumps(doc),
            retain=True,
            hostname=MQTT_BROKER
        )
    except Exception as error:
        print("Could not publish the settings:", error)
        
    return doc

#THE FIFTH TASK (A5)


@app.delete("/fans/{mac}")
def delete_fan(mac: str, response: Response, x_api_key: str | None = Header(default=None, alias="X-API-Key")):
    fan = fans.find_one({"mac": mac})
    if not fan:
        raise HTTPException(status_code=404, detail="Fan not found")

    if not x_api_key:
        raise HTTPException(status_code=401, detail="API key is missing")

    key_owner = fans.find_one({"key": x_api_key})
    if not key_owner:
        raise HTTPException(status_code=401, detail="Invalid API key")

    if key_owner["mac"] != mac:
        raise HTTPException(status_code=403, detail="API key belongs to another fan")

    fans.delete_one({"mac": mac})
    settings.delete_one({"mac": mac})
    
    response.status_code = status.HTTP_204_NO_CONTENT
    return None
