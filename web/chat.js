"use strict";

document.addEventListener("DOMContentLoaded", () => {
    const form =
        document.getElementById("chat-form");

    const input =
        document.getElementById("chat-input");

    const messages =
        document.getElementById("chat-messages");

    const sendButton =
        document.getElementById("send-message");

    const stopButton =
        document.getElementById("stop-generating");

    const generatingMessage =
        document.getElementById("generating-message");

    const composerMenu =
        document.getElementById("composer-menu");

    const cheatsheetsModal =
        document.getElementById("cheatsheets-modal");

    let generationActive = false;

    function scrollMessagesToBottom() {
        messages.scrollTop =
            messages.scrollHeight;
    }

    function resizeInput() {
        input.style.height = "auto";

        const maxHeight = 180;

        const nextHeight = Math.min(
            input.scrollHeight,
            maxHeight
        );

        input.style.height =
            `${nextHeight}px`;

        input.style.overflowY =
            input.scrollHeight > maxHeight
                ? "auto"
                : "hidden";
    }

    function createMessageElement({
                                      role,
                                      content,
                                  }) {
        const article =
            document.createElement("article");

        article.className =
            `message message--${role}`;

        const bubble =
            document.createElement("div");

        bubble.className =
            "message__bubble";

        const contentElement =
            document.createElement("div");

        contentElement.className =
            role === "assistant"
                ? "message__content markdown"
                : "message__content message__content--plain";

        contentElement.textContent = content;

        bubble.append(contentElement);
        article.append(bubble);

        return article;
    }

    function appendUserMessage(content) {
        const message =
            createMessageElement({
                role: "user",
                content,
            });

        messages.append(message);
        scrollMessagesToBottom();
    }

    function appendAssistantMessage(content) {
        const message =
            createMessageElement({
                role: "assistant",
                content,
            });

        messages.append(message);
        scrollMessagesToBottom();
    }

    function setGenerationState(active) {
        generationActive = active;

        sendButton.hidden = active;
        stopButton.hidden = !active;
        input.disabled = active;

        if (active) {
            messages.append(generatingMessage);
            generatingMessage.hidden = false;

            scrollMessagesToBottom();
            return;
        }

        generatingMessage.hidden = true;
        input.focus();
    }

    function completeGeneration(content) {
        setGenerationState(false);
        appendAssistantMessage(content);
    }

    function failGeneration(message) {
        setGenerationState(false);

        appendAssistantMessage(
            message ??
            "Не удалось получить ответ. Попробуйте ещё раз."
        );
    }

    function closeComposerMenu() {
        composerMenu.open = false;
    }

    function openCheatsheets() {
        cheatsheetsModal.hidden = false;
        closeComposerMenu();

        cheatsheetsModal
            .querySelector(".modal__close")
            ?.focus();
    }

    function closeCheatsheets() {
        cheatsheetsModal.hidden = true;
        input.focus();
    }

    function clearInput() {
        input.value = "";
        resizeInput();
    }

    function submitMessage() {
        const message =
            input.value.trim();

        if (
            message.length === 0 ||
            generationActive
        ) {
            return;
        }

        appendUserMessage(message);
        clearInput();
        setGenerationState(true);

        window.dispatchEvent(
            new CustomEvent(
                "chat:message-submit",
                {
                    detail: {
                        message,
                    },
                }
            )
        );
    }

    form.addEventListener(
        "submit",
        (event) => {
            event.preventDefault();
            submitMessage();
        }
    );

    input.addEventListener(
        "input",
        resizeInput
    );

    input.addEventListener(
        "keydown",
        (event) => {
            if (
                event.key !== "Enter" ||
                event.isComposing
            ) {
                return;
            }

            event.preventDefault();
            submitMessage();
        }
    );

    stopButton.addEventListener(
        "click",
        () => {
            window.dispatchEvent(
                new CustomEvent(
                    "chat:generation-stop"
                )
            );

            setGenerationState(false);
        }
    );

    document.addEventListener(
        "click",
        (event) => {
            if (
                composerMenu.open &&
                !composerMenu.contains(event.target)
            ) {
                closeComposerMenu();
            }
        }
    );

    document
        .querySelector(
            '[data-action="open-cheatsheets"]'
        )
        ?.addEventListener(
            "click",
            openCheatsheets
        );

    document
        .querySelectorAll(
            '[data-action="close-cheatsheets"]'
        )
        .forEach((button) => {
            button.addEventListener(
                "click",
                closeCheatsheets
            );
        });

    document
        .querySelectorAll(
            "[data-cheatsheet-text]"
        )
        .forEach((button) => {
            button.addEventListener(
                "click",
                () => {
                    input.value =
                        button.textContent.trim();

                    resizeInput();
                    closeCheatsheets();
                }
            );
        });

    document.addEventListener(
        "keydown",
        (event) => {
            if (event.key !== "Escape") {
                return;
            }

            closeComposerMenu();

            if (!cheatsheetsModal.hidden) {
                closeCheatsheets();
            }
        }
    );

    resizeInput();

    window.chatPage = Object.freeze({
        setGenerationState,
        appendUserMessage,
        appendAssistantMessage,
        completeGeneration,
        failGeneration,
    });
});