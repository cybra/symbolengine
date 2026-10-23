// The Filter's editor: knobs dragged up and down, the mode chosen with buttons, and the output's level on a meter.

(async () => {
    const parameters = await plugin.parameters();

    // A knob's position from 0 to 1. Cutoff moves by octaves, so each turn of the knob sounds as far as the last.
    const shareOf = (parameter, logarithmic) =>
        logarithmic ? Math.log(parameter.value / parameter.minimum) / Math.log(parameter.maximum / parameter.minimum)
                    : parameter.share;

    const valueAt = (parameter, logarithmic, share) => {
        share = Math.min(Math.max(share, 0), 1);
        return logarithmic ? parameter.minimum * Math.pow(parameter.maximum / parameter.minimum, share)
                           : parameter.valueAt(share);
    };

    // The arc a knob draws, from seven o'clock round to five o'clock.
    const arc = share => {
        const start = 0.75 * Math.PI, sweep = 1.5 * Math.PI * share;
        const point = angle => `${50 + 38 * Math.cos(angle)} ${50 + 38 * Math.sin(angle)}`;
        return `M ${point(start)} A 38 38 0 ${sweep > Math.PI ? 1 : 0} 1 ${point(start + sweep)}`;
    };

    for (const knob of document.querySelectorAll(".knob")) {
        const parameter = parameters.get(Number(knob.dataset.parameter));
        const logarithmic = knob.dataset.scale === "log";

        const show = () => {
            const share = shareOf(parameter, logarithmic);
            knob.querySelector(".arc").setAttribute("d", arc(Math.max(share, 0.001)));
            knob.querySelector(".pointer").setAttribute("transform", `rotate(${-135 + 270 * share} 50 50)`);
            knob.querySelector(".value").textContent = parameter.text;
        };

        knob.querySelector(".name").textContent = parameter.name;
        parameter.onChange(show);
        show();

        // A drag of 200 pixels covers the whole range, and Shift makes it finer.
        knob.addEventListener("pointerdown", event => {
            knob.setPointerCapture(event.pointerId);
            const startY = event.clientY, startShare = shareOf(parameter, logarithmic);
            parameter.begin();

            const move = moved => {
                const pixels = moved.shiftKey ? 2000 : 200;
                parameter.set(valueAt(parameter, logarithmic, startShare + (startY - moved.clientY) / pixels));
            };
            const up = () => {
                knob.removeEventListener("pointermove", move);
                parameter.end();
            };
            knob.addEventListener("pointermove", move);
            knob.addEventListener("pointerup", up, { once: true });
            knob.addEventListener("pointercancel", up, { once: true });
        });

        knob.addEventListener("dblclick", () => parameter.change(parameter.default));
    }

    const modes = document.querySelector(".modes");
    const mode = parameters.get(Number(modes.dataset.parameter));
    const buttons = mode.steps.map(step => {
        const button = document.createElement("button");
        button.textContent = step.text;
        button.addEventListener("click", () => mode.change(step.value));
        modes.append(button);
        return button;
    });
    const showMode = () => buttons.forEach((button, i) => button.classList.toggle("chosen", mode.steps[i].value === mode.value));
    mode.onChange(showMode);
    showMode();

    // Each bar falls at 24 dB a second after a peak, over a range of 60 dB.
    const bars = document.querySelectorAll(".meter .bar");
    const shown = [-60, -60];
    let last = performance.now();
    symbolengine.on("levels", levels => {
        const now = performance.now(), fall = 24 * (now - last) / 1000;
        last = now;
        levels.forEach((level, i) => {
            const decibels = level > 0 ? 20 * Math.log10(level) : -60;
            shown[i] = Math.max(decibels, shown[i] - fall, -60);
            bars[i]?.style.setProperty("--level", `${(shown[i] + 60) / 60 * 100}%`);
        });
    });
})();
