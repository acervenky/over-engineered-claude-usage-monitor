import streamDeck from "@elgato/streamdeck";
import { FiveHourAction } from "./actions/five-hour.js";
import { SevenDayAction } from "./actions/seven-day.js";
import { startServer } from "./server.js";

// Start local HTTP server for extension push
startServer();

// Register actions
streamDeck.actions.registerAction(new FiveHourAction());
streamDeck.actions.registerAction(new SevenDayAction());

// Connect to Stream Deck
streamDeck.connect();
