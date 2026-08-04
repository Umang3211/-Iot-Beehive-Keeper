"use client";

import { useEffect, useMemo, useRef, useState } from "react";
import Reveal from "@/components/Reveal";
import { createResponder, typingDelay } from "@/lib/chat";
import { chatGreeting, chatSuggestions, person } from "@/lib/content";

type Message = { id: number; from: "him" | "guest"; text: string };

export default function Chat() {
  const respond = useMemo(() => createResponder(), []);
  const [messages, setMessages] = useState<Message[]>([
    { id: 0, from: "him", text: chatGreeting },
  ]);
  const [draft, setDraft] = useState("");
  const [typing, setTyping] = useState(false);

  const scrollerRef = useRef<HTMLDivElement>(null);
  const nextId = useRef(1);
  const timers = useRef<number[]>([]);

  // Keep the newest message in view without yanking the whole page around.
  useEffect(() => {
    const scroller = scrollerRef.current;
    if (scroller) scroller.scrollTop = scroller.scrollHeight;
  }, [messages, typing]);

  useEffect(() => {
    const pending = timers.current;
    return () => pending.forEach((id) => window.clearTimeout(id));
  }, []);

  function send(text: string) {
    const trimmed = text.trim();
    if (!trimmed || typing) return;

    setMessages((current) => [...current, { id: nextId.current++, from: "guest", text: trimmed }]);
    setDraft("");
    setTyping(true);

    const reply = respond(trimmed);
    const timer = window.setTimeout(() => {
      setTyping(false);
      setMessages((current) => [...current, { id: nextId.current++, from: "him", text: reply }]);
    }, typingDelay(reply));
    timers.current.push(timer);
  }

  const initials = person.name
    .split(" ")
    .map((part) => part[0])
    .join("")
    .slice(0, 2);

  return (
    <section id="chat" className="relative scroll-mt-20 py-24 sm:py-28">
      <div className="shell">
        <Reveal className="text-center">
          <p className="eyebrow">Ask him anything</p>
          <h2 className="section-title mt-3 text-balance">Chat with {person.shortName}</h2>
          <p className="prose-warm mx-auto mt-4 max-w-lg text-balance">
            Not the real one, he is busy. This is fifty years of his greatest hits, running entirely
            in your browser. Nothing you type here is sent anywhere.
          </p>
        </Reveal>

        <Reveal delay={90} className="mx-auto mt-10 max-w-2xl">
          <div className="card-glass overflow-hidden">
            <div className="flex items-center gap-3 border-b border-white/10 bg-white/[0.03] px-5 py-4">
              <div className="flex h-11 w-11 items-center justify-center rounded-full bg-gradient-to-br from-gold-400 to-ember font-display text-base font-bold text-ink-900">
                {initials}
              </div>
              <div>
                <p className="font-body text-sm font-bold text-cream">{person.name}</p>
                <p className="flex items-center gap-1.5 text-xs text-muted">
                  <span className="inline-block h-1.5 w-1.5 rounded-full bg-green-400" />
                  {typing ? "typing" : `${person.milestone} years young`}
                </p>
              </div>
            </div>

            <div
              ref={scrollerRef}
              className="thin-scroll h-[22rem] space-y-3 overflow-y-auto px-4 py-5 sm:px-5"
              aria-live="polite"
            >
              {messages.map((message) => (
                <div
                  key={message.id}
                  className={`bubble flex ${message.from === "guest" ? "justify-end" : "justify-start"}`}
                >
                  <p
                    className={`max-w-[82%] rounded-2xl px-4 py-2.5 text-sm leading-relaxed ${
                      message.from === "guest"
                        ? "rounded-br-sm bg-gold-400 text-ink-900"
                        : "rounded-bl-sm bg-white/[0.07] text-cream"
                    }`}
                  >
                    {message.text}
                  </p>
                </div>
              ))}

              {typing && (
                <div className="bubble flex justify-start">
                  <span className="flex gap-1 rounded-2xl rounded-bl-sm bg-white/[0.07] px-4 py-3.5">
                    <i className="typing-dot h-1.5 w-1.5 rounded-full bg-cream" />
                    <i className="typing-dot h-1.5 w-1.5 rounded-full bg-cream" />
                    <i className="typing-dot h-1.5 w-1.5 rounded-full bg-cream" />
                  </span>
                </div>
              )}
            </div>

            <div className="border-t border-white/10 px-4 py-3 sm:px-5">
              <div className="thin-scroll mb-3 flex gap-2 overflow-x-auto pb-1">
                {chatSuggestions.map((suggestion) => (
                  <button
                    key={suggestion}
                    onClick={() => send(suggestion)}
                    disabled={typing}
                    className="flex-none rounded-full border border-white/[0.12] px-3 py-1.5 text-xs text-muted transition-colors hover:border-gold-400/60 hover:text-cream disabled:opacity-40"
                  >
                    {suggestion}
                  </button>
                ))}
              </div>

              <form
                onSubmit={(event) => {
                  event.preventDefault();
                  send(draft);
                }}
                className="flex gap-2"
              >
                <input
                  className="field flex-1"
                  value={draft}
                  maxLength={300}
                  onChange={(event) => setDraft(event.target.value)}
                  placeholder={`Say something to ${person.shortName}`}
                  aria-label={`Message ${person.name}`}
                />
                <button type="submit" className="btn-gold px-5" disabled={!draft.trim() || typing}>
                  Send
                </button>
              </form>
            </div>
          </div>
        </Reveal>
      </div>
    </section>
  );
}
