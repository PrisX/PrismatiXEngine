import {serializeStoryCommand, type JsonValue} from "@prismatix/authoring-sdk";

/**
 * Story command spellings for importers. Every short form here compiles to
 * the same Runtime IR operation (kind and arguments) as its long named-argument
 * form, so switching between them never moves a save. Forms that would change
 * the IR are deliberately absent: omitting `center` from [show] or `channel=1`
 * from [se] drops an argument, inline `[w]` and merged waits fold operations
 * together, `[face]` emits a different show, and project macros give expanded
 * operations new `/mN` identities.
 */

const bareToken = /^[^\s\u001c-\u001f\u0085[\]{}"'=]+$/u;
const labelToken = /^[A-Za-z0-9_][A-Za-z0-9_.-]*$/u;

/** A command argument value, quoted only when the Story tokenizer needs it. */
export function storyToken(value: string): string {
  return bareToken.test(value) && !value.includes("﻿") ? value : JSON.stringify(value);
}

/** `[name key=json ...]` through the SDK's checked serializer. */
export function namedCommand(name: string, args: Readonly<Record<string, JsonValue>> = {}): string {
  const serialized = serializeStoryCommand(name, args);
  if (!serialized.valid || serialized.value === undefined)
    throw new Error(`Cannot write [${name}]: ${serialized.diagnostics.map((item) => item.message).join("; ")}`);
  return serialized.value;
}

export class StoryForms {
  private readonly pairs = new Map<string, string>();

  /** `long` writes every argument by name, as the first importers did. */
  constructor(readonly long = false) {}

  /** True when two spellings are the long and short form of one command. */
  equivalent(previous: string, current: string): boolean {
    return previous === current || this.pairs.get(current) === previous || this.pairs.get(previous) === current;
  }

  private pick(long: string, short: string): string {
    if (long !== short) { this.pairs.set(long, short); this.pairs.set(short, long); }
    return this.long ? long : short;
  }

  wait(duration: string): string {
    return this.pick(namedCommand("wait", {duration}), `[wait ${storyToken(duration)}]`);
  }

  hud(visible: boolean, time?: string): string {
    return this.pick(namedCommand("hud", {visible, ...(time === undefined ? {} : {time})}),
      `[hud ${visible}${time === undefined ? "" : ` ${storyToken(time)}`}]`);
  }

  unlock(kind: string, id: string): string {
    const long = namedCommand("unlock", {kind, id});
    return this.pick(long, labelToken.test(kind) && labelToken.test(id) ? `[unlock ${kind} ${id}]` : long);
  }

  chapter(title: string): string {
    const long = namedCommand("chapter", {title});
    return this.pick(long, bareToken.test(title) ? `[chapter ${title}]` : long);
  }

  set(name: string, value: number): string {
    const long = namedCommand("set", {name, value});
    return this.pick(long, labelToken.test(name) ? `[set ${name}=${value}]` : long);
  }

  add(name: string, amount: number): string {
    const long = namedCommand("add", {name, amount});
    return this.pick(long, labelToken.test(name) ? `[add ${name} ${amount}]` : long);
  }

  /** [jump]/[call] to a label, another scene, or a label in another scene. */
  transfer(kind: "jump" | "call", scene?: string, target?: string): string {
    return this.pick(namedCommand(kind, {...(scene === undefined ? {} : {scene}),
      ...(target === undefined ? {} : {target})}),
    `[${kind}${target === undefined ? "" : ` ${storyToken(target)}`}${
      scene === undefined ? "" : ` scene=${storyToken(scene)}`}]`);
  }

  choice(text: string, goto: string, scene?: string): string {
    return this.pick(namedCommand("choice", {text, goto, ...(scene === undefined ? {} : {scene})}),
      `[choice text=${storyToken(text)} goto=${storyToken(goto)}${
        scene === undefined ? "" : ` scene=${storyToken(scene)}`}]`);
  }

  tone(mode: string): string {
    return this.pick(namedCommand("tone", {mode}), `[tone ${storyToken(mode)}]`);
  }

  /** [bgm.stop]/[ambience.stop] with an optional fade. */
  stopMusic(command: "bgm.stop" | "ambience.stop", fade?: string): string {
    if (fade === undefined) return `[${command}]`;
    return this.pick(`[${command} fade=${fade}]`, `[${command} ${storyToken(fade)}]`);
  }

  stopSound(channel?: number, fade?: string): string {
    const long = `[se.stop${channel === undefined ? "" : ` channel=${channel}`}${fade === undefined ? "" : ` fade=${fade}`}]`;
    return fade === undefined ? long
      : this.pick(long, `[se.stop ${storyToken(fade)}${channel === undefined ? "" : ` channel=${channel}`}]`);
  }

  /** `color` is six hex digits, `alpha` the exact text the IR should carry. */
  flash(color: string, alpha: string, duration: string, loop: number): string {
    return this.pick(`[flash color=${color} alpha=${alpha} duration=${duration} loop=${loop}]`,
      `[flash ${storyToken(duration)}${color === "ffffff" ? "" : ` color=${color}`} alpha=${alpha}${
        loop === 1 ? "" : ` loop=${loop}`}]`);
  }
}
