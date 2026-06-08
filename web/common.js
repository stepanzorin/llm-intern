"use strict";

(function () {
    const storageKeys = Object.freeze({
        theme: "intern-theme",
    });

    const subscriptionPlans = Object.freeze({
        FREE: Object.freeze({
            code: "FREE",
            name: "Базовая",
        }),

        PLUS: Object.freeze({
            code: "PLUS",
            name: "Стандартная",
        }),

        PREMIUM: Object.freeze({
            code: "PREMIUM",
            name: "Наивысшая",
        }),
    });

    function normalizeSubscriptionPlan(value) {
        const normalized = String(value ?? "")
            .trim()
            .toUpperCase();

        if (Object.hasOwn(subscriptionPlans, normalized)) {
            return subscriptionPlans[normalized];
        }

        return subscriptionPlans.FREE;
    }

    function subscriptionPlanLabel(value) {
        const plan = normalizeSubscriptionPlan(value);

        return `${plan.code} · ${plan.name}`;
    }

    function applySubscriptionLabels(root = document) {
        root
            .querySelectorAll("[data-subscription-plan]")
            .forEach((element) => {
                const plan = normalizeSubscriptionPlan(
                    element.dataset.subscriptionPlan
                );

                element.dataset.subscriptionPlan =
                    plan.code.toLowerCase();

                element.textContent =
                    `${plan.code} · ${plan.name}`;
            });
    }

    function resolveSystemTheme() {
        return window
            .matchMedia("(prefers-color-scheme: dark)")
            .matches
            ? "dark"
            : "light";
    }

    function applyStoredTheme() {
        const root = document.documentElement;

        const savedTheme =
            localStorage.getItem(storageKeys.theme) ??
            "light";

        if (savedTheme === "system") {
            root.dataset.theme = resolveSystemTheme();
            return;
        }

        if (savedTheme === "dark") {
            root.dataset.theme = "dark";
            return;
        }

        if (savedTheme === "custom") {
            root.dataset.theme = "custom";
            return;
        }

        root.dataset.theme = "light";
    }

    function closeSystemBanner(button) {
        const banner = button.closest(".system-banner");

        if (banner !== null) {
            banner.hidden = true;
        }
    }

    function goBack() {
        if (window.history.length > 1) {
            window.history.back();
            return;
        }

        window.location.href = "./index.html";
    }

    document.addEventListener("DOMContentLoaded", () => {
        applyStoredTheme();
        applySubscriptionLabels();

        document
            .querySelectorAll(
                '[data-action="close-system-banner"]'
            )
            .forEach((button) => {
                button.addEventListener("click", () => {
                    closeSystemBanner(button);
                });
            });

        document
            .querySelectorAll('[data-action="go-back"]')
            .forEach((button) => {
                button.addEventListener(
                    "click",
                    goBack
                );
            });
    });

    window
        .matchMedia("(prefers-color-scheme: dark)")
        .addEventListener("change", () => {
            const savedTheme =
                localStorage.getItem(storageKeys.theme) ??
                "light";

            if (savedTheme === "system") {
                applyStoredTheme();
            }
        });

    window.internUi = Object.freeze({
        storageKeys,
        subscriptionPlans,
        normalizeSubscriptionPlan,
        subscriptionPlanLabel,
        applySubscriptionLabels,
        applyStoredTheme,
        resolveSystemTheme,
    });
})();