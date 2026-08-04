import { Intent, fallbackReplies, intents } from "@/lib/content";

/**
 * A small intent matcher that runs entirely in the browser. No network call, no
 * API key, nothing typed here ever leaves the page.
 *
 * Scoring rewards specificity: a multi word phrase that appears verbatim beats a
 * single keyword, which in turn beats a prefix match. That keeps "happy
 * birthday" landing on the compliment intent instead of the birthday age intent
 * purely because both mention a birthday.
 */

const STOPWORDS = new Set([
  "the", "a", "an", "is", "are", "was", "were", "do", "does", "did", "you", "your",
  "yours", "me", "my", "i", "to", "of", "and", "or", "in", "on", "for", "it", "that",
  "this", "what", "whats", "how", "why", "when", "who", "can", "would", "will", "be",
  "have", "has", "had", "with", "about", "tell", "give", "get", "any", "some",
]);

function tokenize(input: string): string[] {
  return input
    .toLowerCase()
    .replace(/[^a-z0-9\s]/g, " ")
    .split(/\s+/)
    .filter((word) => word.length > 1 && !STOPWORDS.has(word));
}

function scoreIntent(intent: Intent, normalized: string, tokens: string[]): number {
  let score = 0;

  for (const raw of intent.keywords) {
    const keyword = raw.toLowerCase();

    if (keyword.includes(" ")) {
      if (normalized.includes(keyword)) score += 4;
      continue;
    }
    if (tokens.includes(keyword)) {
      score += 2;
      continue;
    }
    if (keyword.length >= 4 && tokens.some((t) => t.startsWith(keyword) || keyword.startsWith(t))) {
      score += 1;
    }
  }
  return score;
}

function pickUnused(options: string[], used: Set<string>): string {
  const fresh = options.filter((option) => !used.has(option));
  const pool = fresh.length > 0 ? fresh : options;
  const choice = pool[Math.floor(Math.random() * pool.length)];

  // Once every line in a group has been used, forget the group so it can cycle
  // again rather than locking onto one answer forever.
  if (fresh.length <= 1) {
    for (const option of options) used.delete(option);
  }
  used.add(choice);
  return choice;
}

export function createResponder() {
  const used = new Set<string>();

  return function respond(input: string): string {
    const normalized = input.toLowerCase().trim();
    const tokens = tokenize(normalized);

    let best: Intent | null = null;
    let bestScore = 0;

    for (const intent of intents) {
      const score = scoreIntent(intent, normalized, tokens);
      if (score > bestScore) {
        best = intent;
        bestScore = score;
      }
    }

    if (!best || bestScore < 2) return pickUnused(fallbackReplies, used);
    return pickUnused(best.replies, used);
  };
}

/** Rough thinking time so replies do not snap back instantly. */
export function typingDelay(reply: string): number {
  return Math.min(2200, 500 + reply.length * 12);
}
