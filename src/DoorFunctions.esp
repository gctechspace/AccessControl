// Door Specific Functions go here.
#ifdef DOOR

void handleCard(long tagId) {
  authcard(tagId);
}
void authCard(long tagId) {
  if (useLocal != 1) {
    log("[AUTH] Server auth check begin");
    String url = String(host) + "/api/" + deviceType + "/check/" + String(tagid) + "/?secret=" + String(secret);
    log("[AUTH] Get:" + String(url));
    std::unique_ptr<BearSSL::WiFiClientSecure>SSLclient(new BearSSL::WiFiClientSecure);
    SSLclient->setInsecure();
    client.begin(*SSLclient, url);

    // Start http request.
    int httpCode = client.GET();
    // httpCode will be negative on error
    if (httpCode > 0) {
      log("[AUTH] Code: " + String(httpCode));

      // Checkin succeeded.
      if (httpCode == HTTP_CODE_OK) {
        String payload = client.getString();
        log("[AUTH] Server response: " + payload);
        const size_t capacity = JSON_OBJECT_SIZE(4) + 90;
        DynamicJsonDocument doc(capacity);
        deserializeJson(doc, payload);
        if ( doc["access"].as<String>() == "true" ) {
          log("[AUTH] Access granted.");
          pulseContact();
          lastId = tagid;
        } else {
          log("[AUTH] Access not granted.");
          delay(1000);
        }
        // Clear the json object now.
        doc.clear();
      }
    } else {
      log("[AUTH] Error: " + client.errorToString(httpCode));
    }
    client.end();
    log("[AUTH] Card Auth done.");
    delay(10);
  } else {
    if (tagsArray[0] > 0) {
      log("[AUTH] Checking local cache.");
      for (uint8_t i = 0; i < sizeof(tagsArray); i++) {
        if (tagsArray[i] == tagid) {
          log("[AUTH] Cache Access granted.");
          sessionID = "Cache in use";
          pulseContact();
          lastId = tagid;
          break;
        } else {
          if (tagsArray[i] < 1) {
            // Check to see if this is the last tag in the cache, if so error.
            log("[AUTH] Cached Access not granted.");
            delay(1000);
            break;
          }
        }
      }
    } else {
      log("[AUTH] Cache check fail, no tags loaded");
    }
  }
}

void handleTag() {
  char tagBytes[6];

  //  while (!Serial.available()) { delay(10); }

  if (Serial.readBytes(tagBytes, 5) == 5)
  {
    uint8_t checksum = 0;
    uint32_t cardId = 0;

    tagBytes[6] = 0;

    //    log("Raw Tag:");
    for (int i = 0; i < 4; i++)
    {
      checksum ^= tagBytes[i];
      cardId = cardId << 8 | tagBytes[i];
      //     Serial.println(tagBytes[i], HEX);
    }

    if (checksum == tagBytes[4])
    {
      log("[AUTH] Tag Number:" + String(cardId));
      flushSerial();
      authCard(cardId);
      lastReadSuccess = millis();
    } else {
      flushSerial();
      log("[AUTH] incomplete or corrupted RFID read, sorry. ");
    }
  }
}
void pulseContact() {
  switch (contact) {
    case 0:
      {
        digitalWrite(switchPin, HIGH);
        delay(5000);
        digitalWrite(switchPin, LOW);
        break;
      }
    case 1:
      {
        digitalWrite(switchPin, LOW);
        delay(5000);
        digitalWrite(switchPin, HIGH);
        break;
      }
  }
}
void checkStateMachine() {
  // Check to see if any of our state flags have tripped.
  switch (triggerFlag) {
    case 1: // Standard heartbeat with server.
      {
        delay(10);
        checkIn();
        delay(10);
        log("[DEBUG] Free Heap Size: " + String(ESP.getFreeHeap()));
        break;
      }
    case 2: // Session heartbeat for interlocks.
      {
        //Something is seriously wrong if we end up here.
        break;
      }
    case 3: // Our cache is out of date, go get it.
      {
        delay(10);
        getCache();
        delay(10);
        triggerFlag = 0;
        break;
      }
    case 4: // If we reach this state, a heartbeat has failed. Load tags, mark as loaded and tell system to use local for next auths.
      {
        if (useLocal < 1) {

          log("[CACHE] Loading local cache.");
          loadTags();
          delay(10);
          tagsLoaded = 1;
          useLocal = 1;
          triggerFlag = 0;
          break;
        } else {
          log("[CACHE] Heartbeat still failed.  ");
          triggerFlag = 0;
          break;
        }
      }
    case 5: // If we reach this state, a heartbeat has succeeded after being in local mode.
      {
        log("[CACHE] Unloading local cache.");
        clearTags();
        delay(10);
        triggerFlag = 0;
        break;
      }
  }
}
#endif
