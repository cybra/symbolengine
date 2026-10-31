// symbolengine.js: the page's side of the bridge to the native code, served to every page.

(() => {
    if (window.symbolengine)
        return;

    const handlers = new Map();

    // The native function is bound once the web view is ready, which on some systems is after the page has started
    // loading, so the first call waits for it.
    const bound = new Promise(resolve => {
        const look = () => typeof window.__symbolengine_call === "function" ? resolve() : setTimeout(look, 5);
        look();
    });

    // Calls whose answers come later, by number, each waiting from before it is made, so an answer that comes before
    // the call's own reply still finds it.
    let calls = 0;
    const later = new Map();

    const callNative = async (name, args) => {
        await bound;
        const number = ++calls;
        const answer = new Promise((resolve, reject) => later.set(number, { resolve, reject }));
        const reply = JSON.parse(await window.__symbolengine_call(name, JSON.stringify(args), number));
        if (reply.later)
            return answer;
        later.delete(number);
        if ("error" in reply)
            throw new Error(reply.error);
        return reply.value;
    };

    window.__symbolengine_answer = (number, reply) => {
        const waiting = later.get(number);
        later.delete(number);
        if (waiting && "error" in reply)
            waiting.reject(new Error(reply.error));
        else if (waiting)
            waiting.resolve(reply.value);
    };

    window.__symbolengine_receive = (event, data) => {
        for (const handler of handlers.get(event) ?? []) {
            try {
                handler(data);
            } catch (error) {
                console.error(error);
            }
        }
    };

    window.symbolengine = Object.freeze({
        // Calls a function the native side exposed, and gives what it returned.
        call: (name, ...args) => callNative(String(name), args),

        // Opens a web address in the user's browser, as a page itself goes nowhere but its own files. Gives whether
        // the system took it; anything but an http or https address it doesn't.
        openLink: address => callNative("__openLink", [String(address)]),

        // Adds a handler for an event the native side sends, and gives a function that removes it.
        on: (event, handler) => {
            if (!handlers.has(event))
                handlers.set(event, new Set());
            handlers.get(event).add(handler);
            return () => handlers.get(event)?.delete(handler);
        },
    });

    // Tells the native side the page is taking what it sends, once its own scripts have run and added their handlers,
    // so nothing the native side held for it arrives before them.
    if (document.readyState === "loading")
        document.addEventListener("DOMContentLoaded", () => callNative("__ready", []), { once: true });
    else
        callNative("__ready", []);
})();
