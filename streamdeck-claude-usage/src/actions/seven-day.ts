import {
  action,
  SingletonAction,
  WillAppearEvent,
  WillDisappearEvent,
} from "@elgato/streamdeck";
import { store, buildTitle, pushToActions } from "../store.js";

@action({ UUID: "com.venky.claudeusage.sevenday" })
export class SevenDayAction extends SingletonAction {
  private _countdownTimer: ReturnType<typeof setInterval> | null = null;

  override async onWillAppear(ev: WillAppearEvent): Promise<void> {
    store.sevenDayActions.add(ev.action);

    if (store.usage) {
      await ev.action.setTitle(
        buildTitle(store.usage.sevenDay, store.usage.sevenDayResetsAt)
      );
    } else {
      await ev.action.setTitle("7DAY\nwaiting...");
    }

    this._countdownTimer = setInterval(() => pushToActions(), 60_000);
  }

  override onWillDisappear(ev: WillDisappearEvent): void {
    store.sevenDayActions.delete(ev.action);
    if (this._countdownTimer) {
      clearInterval(this._countdownTimer);
      this._countdownTimer = null;
    }
  }
}
