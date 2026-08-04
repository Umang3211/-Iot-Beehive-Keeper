import Reveal from "@/components/Reveal";
import { biography, person, timeline } from "@/lib/content";

export default function Biography() {
  return (
    <section id="story" className="relative py-24 sm:py-32">
      <div className="shell">
        <Reveal>
          <p className="eyebrow">The short version</p>
          <h2 className="section-title mt-3 max-w-2xl text-balance">
            Fifty years of {person.shortName}
          </h2>
        </Reveal>

        <div className="mt-12 grid gap-10 lg:grid-cols-[1.1fr_0.9fr] lg:gap-16">
          <Reveal delay={80} className="space-y-5">
            {biography.map((paragraph, i) => (
              <p
                key={i}
                className={
                  i === 0
                    ? "prose-warm text-lg leading-[1.7] text-cream/90 sm:text-xl"
                    : "prose-warm"
                }
              >
                {paragraph}
              </p>
            ))}
          </Reveal>

          <Reveal delay={160}>
            <figure className="card-glass relative p-7 sm:p-9">
              <svg
                aria-hidden
                className="absolute -top-3 left-6 h-12 w-12 text-gold-400/25"
                viewBox="0 0 32 32"
                fill="currentColor"
              >
                <path d="M13 8C8 10 5 14 5 19c0 3 2 5 5 5s5-2 5-5-2-5-4-5c0-2 1-4 3-5l-1-1zm14 0c-5 2-8 6-8 11 0 3 2 5 5 5s5-2 5-5-2-5-4-5c0-2 1-4 3-5l-1-1z" />
              </svg>
              <blockquote className="relative font-display text-xl leading-[1.5] text-cream sm:text-2xl">
                {person.tagline}
              </blockquote>
              <figcaption className="mt-5 text-xs uppercase tracking-[0.18em] text-muted">
                Everyone who has ever met him
              </figcaption>
            </figure>
          </Reveal>
        </div>

        {/* Timeline */}
        <div className="relative mt-24">
          <div
            aria-hidden
            className="timeline-line absolute left-[11px] top-0 h-full w-px sm:left-1/2 sm:-translate-x-px"
          />

          <ol className="space-y-12 sm:space-y-16">
            {timeline.map((item, i) => {
              const rightSide = i % 2 === 1;
              return (
                <Reveal
                  as="li"
                  key={item.year + item.title}
                  delay={40}
                  className="relative pl-10 sm:grid sm:grid-cols-2 sm:gap-12 sm:pl-0"
                >
                  <div
                    aria-hidden
                    className="timeline-node absolute left-0 top-1.5 flex h-[23px] w-[23px] items-center justify-center rounded-full bg-gold-400 text-[0.6rem] font-bold text-ink-900 sm:left-1/2 sm:-translate-x-1/2"
                  >
                    {item.glyph}
                  </div>

                  <div
                    className={
                      rightSide
                        ? "sm:col-start-2 sm:pl-10 sm:text-left"
                        : "sm:col-start-1 sm:pr-10 sm:text-right"
                    }
                  >
                    <p className="font-display text-3xl font-semibold text-gold-400">{item.year}</p>
                    <h3 className="mt-1 font-body text-lg font-bold text-cream">{item.title}</h3>
                    <p className="prose-warm mt-2 text-sm sm:text-base">{item.body}</p>
                  </div>
                </Reveal>
              );
            })}
          </ol>
        </div>
      </div>
    </section>
  );
}
