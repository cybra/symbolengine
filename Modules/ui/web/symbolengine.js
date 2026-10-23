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

    const callNative = async (name, args) => {
        await bound;
        const reply = JSON.parse(await window.__symbolengine_call(name, JSON.stringify(args)));
        if ("error" in reply)
            throw new Error(reply.error);
        return reply.value;
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

        // Adds a handler for an event the native side sends, and gives a function that removes it.
        on: (event, handler) => {
            if (!handlers.has(event))
                handlers.set(event, new Set());
            handlers.get(event).add(handler);
            return () => handlers.get(event)?.delete(handler);
        },
    });

    // Tells the native side the page is taking what it sends.
    callNative("__ready", []);
})();
