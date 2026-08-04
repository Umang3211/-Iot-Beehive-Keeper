/* ============================================================================
 *  EDIT THIS FILE TO PERSONALISE THE SITE
 * ----------------------------------------------------------------------------
 *  Everything the site says about Abhishek lives here: the hero, the biography
 *  timeline, the fun stats, and the answers the chat gives. Change the text
 *  below and the whole site updates. You do not need to touch anything else.
 *
 *  The values shipped here are sensible placeholders so the site looks finished
 *  from the first load. Anything still generic is marked with a TODO comment.
 * ========================================================================== */

export const person = {
  name: "Abhishek Shankar",
  shortName: "Abhishek",
  milestone: 50,
  // TODO: set the real birth year so every timeline year lines up.
  birthYear: 1976,
  // TODO: the actual date of the party, shown in the hero.
  partyDate: "Saturday, the big one",
  tagline: "Fifty years of being the loudest laugh in the room",
  heroIntro:
    "Half a century of bad puns, unreasonably strong opinions about tea, and showing up for people before they thought to ask.",
};

/** Big soft stats under the hero. Keep these playful, not a resume. */
export const factCards: { value: string; label: string }[] = [
  { value: "50", label: "years, and counting" },
  { value: "18,262", label: "days of being early to everything" },
  // TODO: swap these two for something only your family would know.
  { value: "∞", label: "cups of chai" },
  { value: "1", label: "correct way to load a dishwasher" },
];

/** The biography prose. Each string becomes its own paragraph. */
export const biography: string[] = [
  // TODO: replace with the real story. Keep it in his voice, not a LinkedIn bio.
  "Abhishek has never been able to walk past a problem. A wobbling table, a friend going quiet, a plan with a hole in it, he notices, and then he quietly fixes it before anyone else has finished discussing it.",
  "He built a life the same way he builds everything else, patiently and with far too many spare parts kept just in case. Family first, always, but with enough room left over for whoever needed a seat at the table that week.",
  "Ask anyone who knows him and you get the same story with different details. He turned up. He stayed late. He made it funnier than it needed to be. Fifty years in, that is still the whole method.",
];

/** Scroll timeline. Years are absolute so you can put in the real ones. */
export const timeline: { year: string; title: string; body: string; glyph: string }[] = [
  // TODO: these are scaffolding. Replace with the real milestones.
  {
    year: "1976",
    title: "The arrival",
    body: "Loud from the very first day, and honestly not much has changed since.",
    glyph: "★",
  },
  {
    year: "1994",
    title: "Leaving home",
    body: "Packed a bag, took a train, and started the long habit of figuring things out on the way.",
    glyph: "✦",
  },
  {
    year: "2001",
    title: "The one that stuck",
    body: "Met the person who would put up with the puns permanently. Best decision on record.",
    glyph: "❤",
  },
  {
    year: "2008",
    title: "Dad",
    body: "Discovered that all the patience he had been saving up was for exactly this.",
    glyph: "☺",
  },
  {
    year: "2016",
    title: "The workshop years",
    body: "Filled a garage with projects, half of which actually worked, all of which were explained in detail.",
    glyph: "⚙",
  },
  {
    year: "2026",
    title: "Fifty",
    body: "Still the first to arrive, still the last to sit down, still convinced he is not that funny.",
    glyph: "50",
  },
];

/* ----------------------------------------------------------------------------
 *  CHAT PERSONA
 * ----------------------------------------------------------------------------
 *  The chat runs entirely in the browser with no API key and no data leaving
 *  the page. It matches what a guest types against the keywords below and
 *  replies in character. Add intents freely, more keywords means better
 *  matching. Every reply is picked at random from its list so repeat questions
 *  do not give repeat answers.
 * -------------------------------------------------------------------------- */

export type Intent = {
  id: string;
  /** Lowercase keywords or short phrases. More is better. */
  keywords: string[];
  /** Replies chosen at random. Write them the way he actually talks. */
  replies: string[];
};

export const chatGreeting =
  "Arre, you came. Sit, sit. Have you eaten? No, properly eaten, not a packet of something in the car.";

export const chatSuggestions = [
  "How does it feel to turn 50?",
  "What is your secret talent?",
  "Tell me a dad joke",
  "Best advice you have ever given?",
  "What are you like as a dad?",
];

export const intents: Intent[] = [
  {
    id: "age",
    keywords: ["50", "fifty", "turning", "old", "age", "birthday", "feel about", "milestone"],
    replies: [
      "Fifty is just forty with better stories and a worse back. I will take the trade.",
      "Everyone keeps asking if I feel old. I felt old at thirty two when I first heard my own knees. This is fine.",
      "Fifty years and the only thing I know for certain is that the good stuff was almost never the thing I was worrying about.",
    ],
  },
  {
    id: "advice",
    keywords: ["advice", "wisdom", "lesson", "learned", "tip", "secret to", "how do you"],
    replies: [
      "Leave earlier than you need to. That is it. That is the whole philosophy, and it has never once let me down.",
      "Be the person who checks. Checks the tyre, checks the date, checks if someone is actually okay. Costs you nothing.",
      "Nobody remembers what you said at the party. They remember that you stayed to help stack the chairs.",
    ],
  },
  {
    id: "joke",
    keywords: ["joke", "funny", "pun", "laugh", "dad joke", "humour", "humor"],
    replies: [
      "I told my family I was making a website about myself. They said do not let it go to your head. Too late, it is the home page.",
      "People say I have a lot of dad jokes. I say I have exactly the right amount, and the sample size proves it.",
      "Why do I always fix things twice? Because the first time I was just gathering data.",
    ],
  },
  {
    id: "family",
    keywords: ["family", "kids", "children", "son", "daughter", "dad", "father", "wife", "husband", "partner"],
    replies: [
      "Everything else on this page is decoration. The family is the actual thing.",
      "Being a dad taught me that you can be completely right and still lose the argument. Repeatedly.",
      "My children think I do not notice things. I notice everything. I simply choose my moments.",
    ],
  },
  {
    id: "food",
    keywords: ["food", "eat", "cook", "chai", "tea", "coffee", "dinner", "favourite food", "favorite food"],
    replies: [
      "There is a correct way to make chai and there is what most of you are doing. I have made my peace with it. Mostly.",
      "I do not trust anyone who is not hungry. What is that? Where is the joy?",
      "Feed people. When you do not know what to say to someone, feed them. Works every single time.",
    ],
  },
  {
    id: "work",
    keywords: ["work", "job", "career", "office", "build", "project", "engineer", "business"],
    replies: [
      "Every problem is smaller once you actually open it up and look. The dread is always bigger than the repair.",
      "I have started more projects than I have finished, and I regret exactly none of them.",
      "The trick to any job is caring slightly more than is strictly required. People notice, eventually.",
    ],
  },
  {
    id: "hobby",
    keywords: ["hobby", "fun", "weekend", "free time", "talent", "skill", "cricket", "music", "garden", "sport"],
    replies: [
      "My secret talent is falling asleep in eleven seconds, in any chair, at any volume. It is a gift.",
      "Give me a weekend, a garage, and something that is making a noise it should not be making. That is my idea of a holiday.",
      "I can tell you what is wrong with a car by the sound alone. I am right about sixty percent of the time, which I consider excellent.",
    ],
  },
  {
    id: "party",
    keywords: ["party", "celebrate", "tonight", "event", "cake", "speech", "toast"],
    replies: [
      "I said no fuss. Somebody built an entire website. This is the opposite of no fuss.",
      "If there is a speech I will pretend to be embarrassed, and I will remember every word of it for the next decade.",
      "Just make sure the food comes out hot and nobody makes me dance before ten.",
    ],
  },
  {
    id: "compliment",
    keywords: ["love you", "miss you", "amazing", "best", "thank you", "thanks", "legend", "happy birthday", "congrats"],
    replies: [
      "Now you are just trying to make me emotional in front of everyone. It is working. Do not tell them.",
      "Thank you, truly. Now go and get a plate before it all goes.",
      "Fifty years of this and I still do not know how to take a compliment. But I heard it. I always hear it.",
    ],
  },
  {
    id: "photos",
    keywords: ["photo", "picture", "gallery", "upload", "album", "image", "memory", "memories"],
    replies: [
      "Scroll up, the whole wall is up there. Add yours, even the unflattering ones. Especially those.",
      "Every photo on this page has a story that takes ten minutes to tell. Ask me about any of them and find out.",
      "I have no idea who kept all these. I am both touched and slightly concerned.",
    ],
  },
];

/** Used when nothing matches. Should still sound like him. */
export const fallbackReplies: string[] = [
  "Hmm. Ask me that again at the party when I have had a drink and I will give you a proper answer.",
  "You know, in fifty years nobody has asked me that. Let me think about it and get back to you. Have a samosa meanwhile.",
  "I have absolutely no idea, and at my age I have stopped pretending otherwise. Try me on something else.",
  "That is a big question for a birthday website. Try asking about the family, the food, or my terrible jokes.",
];
