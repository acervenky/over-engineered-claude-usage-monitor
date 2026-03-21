import {
  action,
  SingletonAction,
  WillAppearEvent,
  WillDisappearEvent,
} from "@elgato/streamdeck";
import { store, buildTitle, pushToActions } from "../store.js";

@action({ UUID: "com.venky.claudeusage.fivehour" })
export class FiveHourAction extends SingletonAction {
  private _countdownTimer: ReturnType<typeof setInterval> | null = null;

  override async onWillAppear(ev: WillAppearEvent): Promise<void> {
    // Register this action instance
    store.fiveHourActions.add(ev.action);

    // Show current data immediately
    if (store.usage) {
      await ev.action.setTitle(
        buildTitle(store.usage.fiveHour, store.usage.fiveHourResetsAt)
      );
    } else {
      await ev.action.setTitle("5HR\nwaiting...");
    }

    // Tick countdown every 60s
    this._countdownTimer = setInterval(() => pushToActions(), 60_000);
  }

  override onWillDisappear(ev: WillDisappearEvent): void {
    store.fiveHourActions.delete(ev.action);
    if (this._countdownTimer) {
      clearInterval(this._countdownTimer);
      this._countdownTimer = null;
    }
  }
}
