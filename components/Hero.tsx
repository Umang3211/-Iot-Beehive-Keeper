"use client";

import { useEffect, useState } from "react";
import { factCards, person } from "@/lib/content";

type Ember = { left: number; size: number; duration: number; delay: number };

export default function Hero() {
  const [embers, setEmbers] = useState<Ember[]>([]);

  // Generated after mount so the server and client markup match. Randomising
  // during render would produce a hydration mismatch.
  useEffect(() => {
    setEmbers(
      Array.from({ length: 22 }, () => {
        const duration = 11 + Math.random() * 14;
        return {
          left: Math.random() * 100,
          size: 3 + Math.random() * 8,
          duration,
          // Negative offsets start each ember partway through its rise, so the
          // hero is already alive on arrival instead of empty for the first
          // fifteen seconds while they queue up.
          delay: -Math.random() * duration,
        };
      })
    );
  }, []);

  return (
    <header className="relative flex min-h-[100svh] flex-col justify-center overflow-hidden pb-16 pt-24">
      <div aria-hidden className="pointer-events-none absolute inset-0 z-0">
        {embers.map((ember, i) => (
          <span
            key={i}
            className="ember-dot"
            style={{
              left: `${ember.left}%`,
              bottom: "-6vh",
              width: `${ember.size}px`,
              height: `${ember.size}px`,
              animationDuration: `${ember.duration}s`,
              animationDelay: `${ember.delay}s`,
            }}
          />
        ))}
      </div>

      <div className="shell text-center">
        <p className="eyebrow mb-6">{person.partyDate}</p>

        <h1 className="sr-only">
          {person.name} turns {person.milestone}
        </h1>

        <div aria-hidden className="relative">
          <span className="hero-fifty block select-none">{person.milestone}</span>
        </div>

        <p
          className="mx-auto -mt-2 font-display text-3xl font-semibold tracking-tight text-cream sm:text-5xl md:-mt-4"
          style={{ fontVariationSettings: '"SOFT" 60, "WONK" 1' }}
        >
          {person.name}
        </p>

        <p className="prose-warm mx-auto mt-6 max-w-xl text-balance">{person.heroIntro}</p>

        <div className="mt-9 flex flex-wrap items-center justify-center gap-3">
          <a href="#wall" className="btn-gold">
            See the photo wall
          </a>
          <a href="#add" className="btn-ghost">
            Add your photos
          </a>
          <a href="#chat" className="btn-ghost">
            Chat with {person.shortName}
          </a>
        </div>

        <dl className="mx-auto mt-16 grid max-w-3xl grid-cols-2 gap-x-4 gap-y-8 sm:grid-cols-4">
          {factCards.map((fact) => (
            <div key={fact.label}>
              <dt className="font-display text-3xl font-semibold text-gold-400 sm:text-4xl">
                {fact.value}
              </dt>
              <dd className="mt-1 text-[0.7rem] uppercase tracking-[0.14em] text-muted">
                {fact.label}
              </dd>
            </div>
          ))}
        </dl>
      </div>

      <a
        href="#story"
        aria-label="Scroll to the story"
        className="scroll-cue absolute bottom-7 left-1/2 z-10 -translate-x-1/2 text-gold-400"
      >
        <svg width="26" height="26" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="1.8">
          <path d="M12 5v14M5 12l7 7 7-7" strokeLinecap="round" strokeLinejoin="round" />
        </svg>
      </a>
    </header>
  );
}
