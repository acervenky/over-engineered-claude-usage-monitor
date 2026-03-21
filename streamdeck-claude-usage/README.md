# Claude Usage Monitor — Stream Deck Plugin

> Part of the **Over Engineered by Venky** series.

Two Stream Deck buttons. One for your 5-hour Claude usage window. One for your 7-day window. Both show live utilization % and countdown to reset.

---

## How it works

The plugin runs a local HTTP server on `localhost:57891`. The Chrome extension (from the main project) pushes usage data to it every 5 minutes automatically. No credentials stored in the plugin — the extension handles all auth.

```
Chrome Extension → POST localhost:57891/usage → Stream Deck buttons update
```

---

## Setup

### 1. Build the plugin

```bash
npm install
npm run build
```

### 2. Link to Stream Deck

```bash
npx @elgato/cli link com.venky.claudeusage.sdPlugin
```

Or double-click the `.streamDeckPlugin` file if you have a release build.

### 3. Add buttons

Open the Stream Deck app → find **Claude Usage Monitor** in the actions list → drag **5-Hour Window** and **7-Day Window** onto your keys.

### 4. Connect the Chrome extension

In the Chrome extension settings, the Stream Deck endpoint is pre-configured at `http://127.0.0.1:57891`. No setup needed — it pushes automatically on every poll.

---

## Button display

```
┌─────────┐    ┌─────────┐
│  41.0%  │    │  27.0%  │
│  4h 57m │    │  6d 5h  │
└─────────┘    └─────────┘
  5-Hour          7-Day
```

Countdown ticks every 60 seconds between extension polls.

---

## Requirements

- Node.js v20+
- Stream Deck app v6.6+
- Chrome extension from the main project running in your browser
