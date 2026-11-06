// Bayview Forever (Broski280's mod for the PC game, runtime/bf_*.c): every option of its BayviewForever.ini as a
// settings.ini key, with its control and words for the Mods tab.  Data only: settings.ts adds the keys (BF_KEYS) and
// the template's lines (BF_TEMPLATE); mods.ts makes a row per part (BF_PARTS), each with its own options under it;
// settings-ui.ts takes their controls and words (BF_CONTROLS, BF_MORE).
//
// A key is its runtime variable in lower case: NFSU2_BF_ENDLESSCAREER_REGIONS_AIRPORT is
// bf_endlesscareer_regions_airport, the ini's [EndlessCareer.Regions] Airport.  Each part's switch is off by default,
// as every mod here (the original game); its options start at the ini's own values.
// The words are English (N), translated where they are drawn.

import { N } from './i18n';
import type { KeyDef } from './settings';

/** a control of the settings panel (settings-ui.ts Control: the kinds these use) */
export type BfControl =
  | { t: 'switch' }
  | { t: 'range'; min: number; max: number; step: number; unit?: string }
  /** a number box: the options that take a fraction (a slider's key is a whole number) */
  | { t: 'num'; min: number; max: number; step: number; unit?: string }
  | { t: 'seg'; opts: [value: string, label: string][] };

/** an option: its key (and so its variable), its kind, range and default, its control, label and help line */
export interface BfOpt {
  key: string;
  kind: 'bool' | 'int' | 'num' | 'enum';
  vals?: string;
  def: string;
  c: BfControl;
  label: string;
  help: string;
}

const on = (key: string, def: boolean, label: string, help: string): BfOpt =>
  ({ key, kind: 'bool', def: def ? 'on' : 'off', c: { t: 'switch' }, label, help });
const int = (key: string, lo: number, hi: number, def: number, step: number, unit: string, label: string, help: string): BfOpt =>
  ({ key, kind: 'int', vals: `${lo}..${hi}`, def: String(def), c: { t: 'range', min: lo, max: hi, step, unit }, label, help });
const num = (key: string, lo: number, hi: number, def: number, step: number, unit: string, label: string, help: string): BfOpt =>
  ({ key, kind: 'num', def: String(def), c: { t: 'num', min: lo, max: hi, step, unit: unit.trim() }, label, help });

/** a part of the mod: its Mods tab row (mods.ts), its switch and its options */
export interface BfPart {
  id: string;
  name: string;
  kind: 'rules' | 'car';
  online: 'career' | 'you' | 'mine';
  line: string;
  details: string;
  /** its switch */
  key: string;
  /** what the switch's template line says */
  about: string;
  opts: readonly BfOpt[];
  files: readonly string[];
}

const EC = 'bf_endlesscareer_';
export const BF_PARTS: readonly BfPart[] = [
  {
    id: 'bf-endless', name: 'Endless Career', kind: 'rules', online: 'career', key: 'bf_features_endlesscareer',
    line: N('After Caleb, races keep coming back on the map.'),
    details: 'Once Caleb is beaten, every district keeps races on the map: 183 track variants, reverse layouts and tracks the career never used among them, none repeated until all of a district\'s are won. Race lengths, opponents, prize money and money pickups have settings of their own, and tracks not won yet show the X icon. Rachel\'s last message no longer says there is nothing left to do, and before Caleb nothing changes. Its progress is kept in the profile\'s folder (BF).',
    about: 'Races after the career (Stage 6): 183 track variants, the districts\' races on the map',
    files: ['runtime/bf_endless.c', 'runtime/bf.c'],
    opts: [
      int(`${EC}regions_jacksonheights`, 0, 4, 2, 1, '', N('Jackson Heights races'), N('How many races this district keeps on the map.')),
      int(`${EC}regions_citycore`, 0, 4, 2, 1, '', N('City Core races'), N('How many races this district keeps on the map.')),
      int(`${EC}regions_beaconhill`, 0, 4, 2, 1, '', N('Beacon Hill races'), N('How many races this district keeps on the map.')),
      int(`${EC}regions_coalharbor`, 0, 4, 2, 1, '', N('Coal Harbor races'), N('How many races this district keeps on the map.')),
      int(`${EC}regions_airport`, 0, 4, 2, 1, '', N('Airport races'), N('How many races this district keeps on the map.')),
      int(`${EC}laps_lengthscale`, 25, 300, 100, 5, '%', N('Race length'), N('Every race\'s length; the kinds below scale on top of it.')),
      int(`${EC}laps_circuitscale`, 25, 300, 100, 5, '%', N('Circuit length'), N('Circuits: 2 to 10 laps.')),
      int(`${EC}laps_urlscale`, 25, 300, 100, 5, '%', N('URL length'), N('URL races: 2 to 10 laps.')),
      int(`${EC}laps_sprintscale`, 25, 300, 100, 5, '%', N('Sprint length'), N('Sprints\' laps, where a sprint has them.')),
      int(`${EC}laps_dragscale`, 25, 300, 100, 5, '%', N('Drag length'), N('Drag races\' heats.')),
      int(`${EC}laps_driftscale`, 25, 300, 100, 5, '%', N('Drift length'), N('Drift events.')),
      int(`${EC}laps_streetxscale`, 25, 300, 100, 5, '%', N('Street X length'), N('Street X: from the race\'s own laps.')),
      num(`${EC}laps_targetminutes`, 1, 15, 4, 0.5, ' min', N('Circuit time'), N('The laps of a circuit or URL race are worked out to last about this long.')),
      num(`${EC}laps_drifttargetminutes`, 0.5, 15, 2.5, 0.5, ' min', N('Drift time'), N('A stadium or lot drift lasts about this long.')),
      num(`${EC}laps_circuitspeed`, 10, 100, 45, 1, ' m/s', N('Circuit speed'), N('The average speed the laps are worked out with (45 m/s is 162 km/h).')),
      num(`${EC}laps_speedwayspeed`, 10, 100, 50, 1, ' m/s', N('Speedway speed'), N('The same for the speedway tracks.')),
      num(`${EC}laps_driftspeed`, 5, 100, 20, 1, ' m/s', N('Drift speed'), N('The same for drift events.')),
      int(`${EC}ai_carlevel`, 0, 10, 10, 1, '', N('Opponents\' cars'), N('0 to 10: 10 are the cars of the career\'s end.')),
      int(`${EC}ai_easy`, 0, 9, 3, 1, '', N('Skill on Easy'), N('Opponents\' skill, 0 to 9 (9 the game\'s best).')),
      int(`${EC}ai_medium`, 0, 9, 7, 1, '', N('Skill on Medium'), N('Opponents\' skill, 0 to 9 (9 the game\'s best).')),
      int(`${EC}ai_hard`, 0, 9, 9, 1, '', N('Skill on Hard'), N('Opponents\' skill, 0 to 9 (9 the game\'s best).')),
      int(`${EC}ai_circuitoffset`, -9, 9, 0, 1, '', N('Circuit skill'), N('Added to the skill in this kind of race: below 0 easier.')),
      int(`${EC}ai_urloffset`, -9, 9, 0, 1, '', N('URL skill'), N('Added to the skill in this kind of race: below 0 easier.')),
      int(`${EC}ai_sprintoffset`, -9, 9, 0, 1, '', N('Sprint skill'), N('Added to the skill in this kind of race: below 0 easier.')),
      int(`${EC}ai_dragoffset`, -9, 9, 0, 1, '', N('Drag skill'), N('Added to the skill in this kind of race: below 0 easier.')),
      int(`${EC}ai_driftoffset`, -9, 9, 0, 1, '', N('Drift skill'), N('Added to the skill in this kind of race: below 0 easier.')),
      int(`${EC}ai_streetxoffset`, -9, 9, 0, 1, '', N('Street X skill'), N('Added to the skill in this kind of race: below 0 easier.')),
      on(`${EC}map_newtrackx`, true, N('X for new tracks'), N('Tracks not won yet show the X icon, repeats the circle. Off: the game\'s icons.')),
      on(`${EC}pickups_enabled`, true, N('Money pickups'), N('Each race won brings a money pickup back into free roam.')),
      int(`${EC}pickups_cash`, 250, 1000, 500, 50, ' $', N('Pickup cash'), N('What each pickup pays (the career\'s paid $50 to $400).')),
      int(`${EC}pickups_perwin`, 1, 3, 1, 1, '', N('Pickups per win'), N('How many pickups come back for each race won.')),
      num(`${EC}pickups_minspacing`, 0, 1000, 75, 5, ' m', N('Pickup spacing'), N('A pickup comes back no nearer than this to one still out.')),
      int(`${EC}rewards_percent`, 50, 200, 100, 10, '%', N('Prize money'), N('Of each race\'s prize: the race\'s own, raised to stage 5\'s best of its kind; URL races $3,000.')),
      int(`${EC}rewards_growperwin`, 0, 2, 0, 1, '%', N('Prize growth'), N('Each race won after the career adds this to the prizes (0: flat).')),
      int(`${EC}rewards_growmax`, 0, 100, 100, 10, '%', N('Most growth'), N('The prizes grow by at most this much.')),
    ],
  },
  {
    id: 'bf-vgarage', name: 'Virtual Garage', kind: 'rules', online: 'career', key: 'bf_features_virtualgarage',
    line: N('After Caleb, any number of cars behind your 10 slots.'),
    details: 'After the career the garage keeps any number of cars: in the crib\'s car select, UP and DOWN on a slot show your stored cars in it, Enter takes the one shown (the slot\'s car goes into storage) and Back puts everything back. With all 10 slots full the car lot stores the car it replaces, and a new car costs its stage\'s price; the cars it replaced during the career are stored too. Rachel lends you her 350Z, her RX-8 and her cut Evo (finished with factory parts), which no shop may touch. Stored cars are written when the game saves the profile, into its folder (BF).',
    about: 'Unlimited stored cars after the career; Rachel\'s three cars',
    files: ['runtime/bf_vgarage.c', 'runtime/bf.c'],
    opts: [
      on('bf_virtualgarage_prices_enabled', true, N('Cars cost money'), N('With all slots full a new car costs $10,000 to $75,000 by its stage. Off: every car free.')),
      on('bf_virtualgarage_rachel_cars', true, N('Rachel\'s cars'), N('She lends you her 350Z and RX-8 after Caleb, once per profile.')),
      on('bf_virtualgarage_rachel_evo', true, N('Rachel\'s Evo'), N('Her cut Evo too, finished with factory parts.')),
    ],
  },
  {
    id: 'bf-covers', name: 'Stage 6 Covers', kind: 'rules', online: 'career', key: 'bf_features_covers',
    line: N('After Caleb, races won earn magazine and DVD covers.'),
    details: 'After the career, the races you win count for the car you drive: every few wins earn it a magazine or a DVD cover, whose marker is on the map while you drive that car. Covers come back, the one shot longest ago first, and pay by your car\'s stars. Every version of a cover\'s photo is kept in the profile\'s folder (BF). In Career, Magazines or DVDs, UP and DOWN on a cover browse its older photos.',
    about: 'Covers earned per car after the career; older photos kept and browsed',
    files: ['runtime/bf_covers.c', 'runtime/bf.c'],
    opts: [
      int('bf_covers_earn_magazineeverywins', 0, 100, 5, 1, '', N('Wins per magazine'), N('Every this many wins with a car earn it a magazine cover (0: never).')),
      int('bf_covers_earn_dvdeverywins', 0, 100, 10, 1, '', N('Wins per DVD'), N('Every this many wins with a car earn it a DVD cover (0: never).')),
      int('bf_covers_earn_maxwaitingpercar', 1, 30, 3, 1, '', N('Covers waiting per car'), N('At most this many covers wait for one car.')),
      on('bf_covers_earn_message', true, N('Cover messages'), N('A text message says a cover waits.')),
      on('bf_covers_earn_magazinechallenge', true, N('Race to the photographer'), N('A magazine starts with the timed run to the shoot. Off: the shoot is on the map.')),
      int('bf_covers_earn_cashperstar', 250, 1000, 500, 50, ' $', N('Pay per star'), N('A cover pays this times your car\'s stars.')),
      { key: 'bf_covers_viewer_counter', kind: 'enum', vals: '0|2', def: '2', c: { t: 'seg', opts: [['2', N('On')], ['0', N('Off')]] },
        label: N('Photo counter'), help: N('Browsing a cover\'s photos shows "PHOTO 2/3 UP/DOWN".') },
    ],
  },
  {
    id: 'bf-uparts', name: 'Stage 6 Unique Parts', kind: 'rules', online: 'career', key: 'bf_features_uniqueparts',
    line: N('After Caleb, outruns won earn unique parts.'),
    details: 'After the career, the outruns you win count for the car you drive: every few wins earn it a unique part, performance and visual (hoods, rims, spoilers, vinyls, wide body kits) in turn, and they never run out. A part\'s challenge is on the map while you drive that car. It needs Endless Career. The port\'s own rule stays: after Caleb the shops have every unique visual part for every car.',
    about: 'Unique parts earned per car by outrun wins after the career (needs bf_features_endlesscareer)',
    files: ['runtime/bf_uparts.c', 'runtime/bf.c'],
    opts: [
      int('bf_uniqueparts_outrunsperpart', 1, 100, 7, 1, '', N('Outruns per part'), N('Every this many outrun wins with a car earn it a unique part.')),
      on('bf_uniqueparts_racetoshop', true, N('Race to the shop'), N('A part starts with the timed run to the shop. Off: drive into the shop.')),
      on('bf_uniqueparts_message', true, N('Part messages'), N('A text message says a part waits.')),
    ],
  },
  {
    id: 'bf-dragsteer', name: 'Real Drag Steering', kind: 'car', online: 'you', key: 'bf_features_dragsteering',
    line: N('Drag races with full steering, not lane changes.'),
    details: 'Drag races keep the normal steering instead of the slot-car lane changes. More fun, but the airport drags\' barriers are easier to hit, and on the freeway drags you can go the wrong way (follow the Stadium sign). In a drag with other players only your own car steers so. The mod\'s force feedback in drags is left out: no browser drives a wheel\'s motor.',
    about: 'Drag races keep the normal steering',
    files: ['runtime/bf_dragsteer.c'],
    opts: [],
  },
  {
    id: 'bf-raceai', name: 'Race AI Fixes', kind: 'rules', online: 'career', key: 'bf_features_raceai',
    line: N('Far fewer PIT moves and start-line pile-ups.'),
    details: 'The AI racers block and swerve into you far less: no blocking at a race\'s start, a block only on a car well behind, a pass ended only once clear of your nose. Optionally no catch-up (rubber banding) at all; the AI can then feel faster, so you may want a lower difficulty. Races with other players have no AI racers and stay as they are.',
    about: 'The AI blocks and swerves into the player less',
    files: ['runtime/bf_raceai.c'],
    opts: [
      num('bf_raceai_blockmingap', 0, 50, 30, 1, ' m', N('Block gap'), N('A car must be this far behind an AI before it may block it (0: the game).')),
      num('bf_raceai_blockstartdelay', 0, 120, 20, 1, ' s', N('No blocks at the start'), N('No AI blocks anyone in a race\'s first seconds (0: the game).')),
      num('bf_raceai_blockdelay', 0, 30, 1.5, 0.5, ' s', N('Block delay'), N('An AI waits this long after getting ahead of you before it may block.')),
      num('bf_raceai_passcleargap', 0, 10, 5, 0.5, ' m', N('Pass clearance'), N('An AI passing you moves back only this far clear of your nose (0: the game).')),
      on('bf_raceai_blockplayer', true, N('AI blocks you'), N('Off: the AI never blocks you (they still block each other).')),
      on('bf_raceai_disablecatchup', false, N('No catch-up'), N('The AI ignores the Catch Up option (career races use it).')),
    ],
  },
  {
    id: 'bf-respawn', name: 'Respawn at Garage', kind: 'rules', online: 'career', key: 'bf_features_respawnatgarage',
    line: N('Free roam starts at your garage after a load.'),
    details: 'Loading your profile, or Quit to Main Menu, starts free roam at your garage instead of where you last were. Shops and races still put you back where you were. The spot is taken when you drive into your garage, and kept in the profile\'s folder (BF).',
    about: 'A profile load and Quit to Main Menu start free roam at the garage',
    files: ['runtime/bf_respawn.c', 'runtime/bf.c'],
    opts: [],
  },
  {
    id: 'bf-hshifter', name: 'H-Shifter and Clutch', kind: 'car', online: 'you', key: 'bf_features_hshifter',
    line: N('A wheel\'s H-shifter and clutch pedal, as EA cut them.'),
    details: 'Brings back the game\'s cut "manual with clutch" gearbox for a steering wheel with an H-shifter: pick Manual in the game\'s options, and the lever is the gear while the clutch pedal is the clutch. A shift without the clutch still goes in, but the engine bogs down for a moment; neutral needs no clutch. The brake only brakes, and the throttle drives in reverse too. The shifter\'s buttons and the clutch are the wheel\'s as the page knows them (Controls, Controller); with a controller, Manual stays the game\'s.',
    about: 'The game\'s cut manual-with-clutch gearbox, driven by a wheel\'s H-shifter and clutch',
    files: ['runtime/bf_hshifter.c'],
    opts: [
      on('bf_hshifter_gearbox_clutchmode', true, N('Clutch'), N('Manual with clutch. Off: the lever without a clutch.')),
      on('bf_hshifter_gearbox_dragraces', true, N('In drag races'), N('The shifter in drags too (with Real Drag Steering on).')),
      on('bf_hshifter_gearbox_brakeonlybrakes', true, N('Brake only brakes'), N('The lever alone picks reverse, and the throttle drives in it.')),
      int('bf_hshifter_clutch_topdeadzone', 0, 45, 5, 1, '%', N('Clutch top dead zone'), N('The pedal\'s first part still counts as let go.')),
      int('bf_hshifter_clutch_bottomdeadzone', 0, 45, 10, 1, '%', N('Clutch floor dead zone'), N('The pedal\'s last part counts as pressed to the floor.')),
      int('bf_hshifter_shifting_clutchthreshold', 1, 100, 50, 1, '%', N('Clean shift from'), N('How far the clutch must be pressed for a clean shift.')),
      int('bf_hshifter_shifting_bogpower', 0, 100, 0, 5, '%', N('Bog power'), N('The engine\'s power after a shift without the clutch (100: no bog).')),
      int('bf_hshifter_shifting_boghold', 0, 5000, 400, 50, ' ms', N('Bog time'), N('How long the power stays down.')),
      int('bf_hshifter_shifting_bogrecover', 0, 10000, 1000, 100, ' ms', N('Bog recovery'), N('How long the power takes to come back.')),
    ],
  },
  {
    id: 'bf-surround', name: 'Turbo Sounds', kind: 'car', online: 'you', key: 'bf_features_surround',
    line: N('Your turbo\'s spool and blow-off, louder.'),
    details: 'Your turbo\'s spool whistle and blow-off, louder than the game has them, as the mod\'s Surround Sound part turns them up. That part\'s 5.1 and quad sound is not in the browser yet: in the game\'s 5.1 mode its sound system starts without its memory here, so the sound stays stereo.',
    about: 'The turbo\'s spool and blow-off volumes (the mod\'s Surround Sound; its 5.1 / quad mix is not in the browser yet)',
    files: ['runtime/bf_surround.c'],
    opts: [
      int('bf_surround_sounds_blowoffvolume', 0, 400, 150, 10, '%', N('Blow-off volume'), N('The turbo\'s blow-off, of the game\'s.')),
      int('bf_surround_sounds_spoolvolume', 0, 400, 150, 10, '%', N('Spool volume'), N('The turbo\'s spool whistle, of the game\'s.')),
    ],
  },
  {
    id: 'bf-wheel', name: 'Wheel Steering', kind: 'car', online: 'you', key: 'bf_features_wheel',
    line: N('A steering wheel\'s steering, linear and precise.'),
    details: 'For a steering wheel only (a controller and the keyboard stay as they are): the wheel read at full precision, not the game\'s 255 steps, a linear response instead of the game\'s soft curve, and how much the steering narrows with speed. The full lock\'s angle is the wheel set-up\'s (Controls, Controller). The mod\'s force feedback is left out: no browser drives a wheel\'s motor.',
    about: 'A steering wheel\'s full precision, linear response and speed-sensitive lock',
    files: ['runtime/bf_steering.c'],
    opts: [
      on('bf_wheel_steering_fullresolution', true, N('Full precision'), N('The wheel\'s own position, not the game\'s 255 steps.')),
      int('bf_wheel_steering_linearity', 0, 100, 100, 5, '%', N('Linearity'), N('100: linear. 0: the game\'s curve, soft around the centre.')),
      int('bf_wheel_steering_speedsensitivity', 0, 100, 80, 5, '%', N('Speed sensitivity'), N('100: the game\'s lock, 7 degrees at speed. 0: 45 degrees at every speed.')),
    ],
  },
];

/** the parts' switches and options as settings.ts keys */
export const BF_KEYS: readonly KeyDef[] = BF_PARTS.flatMap(p => [
  { key: p.key, env: `NFSU2_${p.key.toUpperCase()}`, kind: 'bool' as const, scope: 'web' as const },
  ...p.opts.map(o => ({ key: o.key, env: `NFSU2_${o.key.toUpperCase()}`, kind: o.kind, vals: o.vals, scope: 'web' as const })),
]);

/** the parts' lines in settings.ini's template (each commented out with its default) */
export const BF_TEMPLATE = `
# Bayview Forever (Broski280's mod for the PC game): its parts, each off by
# default, and their options as its BayviewForever.ini has them (a key is the
# ini's [Section] Key: bf_endlesscareer_regions_airport is
# [EndlessCareer.Regions] Airport).  The Mods tab sets them.
${BF_PARTS.map(p => [`# ${p.name}: ${p.about}.  [NFSU2_${p.key.toUpperCase()}]`, `#${p.key} = off`,
  ...p.opts.map(o => `#${o.key} = ${o.def}`)].join('\n')).join('\n')}
`;

/** the options' controls, by key (settings-ui.ts MOD_CONTROLS) */
export const BF_CONTROLS: Readonly<Record<string, BfControl>> = Object.fromEntries(BF_PARTS.flatMap(p =>
  [[p.key, { t: 'switch' } as BfControl], ...p.opts.map(o => [o.key, o.c] as [string, BfControl])]));

/** the options' words, by key (settings-ui.ts MOD_MORE) */
export const BF_MORE: Readonly<Record<string, { label: string; help: string }>> = Object.fromEntries(BF_PARTS.flatMap(p =>
  p.opts.map(o => [o.key, { label: o.label, help: o.help }])));
