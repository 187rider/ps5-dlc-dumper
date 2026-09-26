/* homebrew.js — Homebrew Launcher entry for PS5 DLC Dumper.
 *
 * Default action starts the RESIDENT WEB UI. That ordering matters:
 * the Homebrew Launcher runs homebrew by hijacking a "big app" process, and a
 * PS5 only runs one big app at a time — so launching homebrew while a game is
 * running closes that game and unmounts its DLC.
 *
 * Correct flow:
 *   1. Launch this from the Homebrew Launcher with NO game running.
 *   2. It goes resident and shows http://<ps5-ip>:8082 as a notification.
 *   3. Start the game, enter the DLC content.
 *   4. Open that URL on your phone/PC and press Dump.
 *
 * The "Dump now" options below are one-shot and assume the game is ALREADY
 * running with its DLC mounted — only useful if you reached the launcher some
 * other way.
 */

async function main() {
    const PAYLOAD = window.workingDir + '/eboot.elf';

    return {
        mainText: 'DLC Dumper',
        secondaryText: 'Start web UI, then dump mounted DLC to USB',

        onclick: async () => {
            return { path: PAYLOAD, args: '' };   // no args = resident web UI
        },

        options: [
            {
                text: 'Dump now — all mounted DLC',
                onclick: async () => {
                    return { path: PAYLOAD, args: '--now' };
                }
            },
            {
                text: 'Dump now — Far Harbor only',
                onclick: async () => {
                    return { path: PAYLOAD, args: 'FALLOUT4DLC00003' };
                }
            },
            {
                text: 'Dump now — Nuka-World only',
                onclick: async () => {
                    return { path: PAYLOAD, args: 'FALLOUT4DLC00006' };
                }
            }
        ]
    };
}
