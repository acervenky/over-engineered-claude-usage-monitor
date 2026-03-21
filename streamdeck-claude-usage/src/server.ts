// Local HTTP server — receives usage data from the Chrome extension
// Listens on localhost:57891
// POST /usage  { fiveHour, sevenDay, fiveHourResetsAt, sevenDayResetsAt, timestamp }
// GET  /status

import * as http from "http";
import { store, pushToActions, UsageSnapshot } from "./store.js";

export const PORT = 57891;

export function startServer(): void {
  const server = http.createServer((req, res) => {
    // CORS — extension needs this
    res.setHeader("Access-Control-Allow-Origin", "*");
    res.setHeader("Access-Control-Allow-Methods", "POST, GET, OPTIONS");
    res.setHeader("Access-Control-Allow-Headers", "Content-Type");

    if (req.method === "OPTIONS") {
      res.writeHead(204);
      res.end();
      return;
    }

    if (req.method === "GET" && req.url === "/status") {
      res.writeHead(200, { "Content-Type": "application/json" });
      res.end(JSON.stringify({ ok: true, usage: store.usage }));
      return;
    }

    if (req.method === "POST" && req.url === "/usage") {
      let body = "";
      req.on("data", (chunk) => (body += chunk));
      req.on("end", async () => {
        try {
          const payload = JSON.parse(body) as UsageSnapshot;
          store.usage = {
            fiveHour:         payload.fiveHour         ?? null,
            sevenDay:         payload.sevenDay         ?? null,
            fiveHourResetsAt: payload.fiveHourResetsAt ?? null,
            sevenDayResetsAt: payload.sevenDayResetsAt ?? null,
            timestamp:        payload.timestamp        ?? Date.now(),
          };
          await pushToActions();
          res.writeHead(200, { "Content-Type": "application/json" });
          res.end(JSON.stringify({ ok: true }));
        } catch {
          res.writeHead(400, { "Content-Type": "application/json" });
          res.end(JSON.stringify({ error: "invalid json" }));
        }
      });
      return;
    }

    res.writeHead(404, { "Content-Type": "application/json" });
    res.end(JSON.stringify({ error: "not found" }));
  });

  server.listen(PORT, "127.0.0.1", () => {
    console.log(`Claude Usage Monitor listening on localhost:${PORT}`);
  });

  server.on("error", (err) => {
    console.error("Server error:", err);
  });
}
