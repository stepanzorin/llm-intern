"use strict";

(() => {
    class ApiError extends Error {
        constructor(
            message,
            {
                status = 0,
                code = "unknown_error",
                payload = null,
            } = {}
        ) {
            super(message);

            this.name = "ApiError";
            this.status = status;
            this.code = code;
            this.payload = payload;
        }
    }

    async function parseResponse(response) {
        if (response.status === 204) {
            return null;
        }

        const contentType =
            response.headers.get("content-type") ?? "";

        if (contentType.includes("application/json")) {
            try {
                return await response.json();
            } catch {
                throw new ApiError(
                    "Сервер вернул некорректный JSON",
                    {
                        status: response.status,
                        code: "invalid_server_json",
                    }
                );
            }
        }

        const text = await response.text();

        if (text.length === 0) {
            return null;
        }

        return {
            message: text,
        };
    }

    async function request(
        path,
        {
            method = "GET",
            headers = {},
            body,
            signal,
        } = {}
    ) {
        const requestHeaders =
            new Headers(headers);

        if (
            body !== undefined &&
            !(body instanceof FormData) &&
            !requestHeaders.has("Content-Type")
        ) {
            requestHeaders.set(
                "Content-Type",
                "application/json; charset=utf-8"
            );
        }

        let response;

        try {
            response = await fetch(
                path,
                {
                    method,
                    headers: requestHeaders,
                    body,
                    signal,
                    cache: "no-store",
                    credentials: "same-origin",
                }
            );
        } catch (error) {
            if (
                error instanceof DOMException &&
                error.name === "AbortError"
            ) {
                throw error;
            }

            throw new ApiError(
                "Не удалось подключиться к локальному серверу приложения",
                {
                    status: 0,
                    code: "connection_failed",
                }
            );
        }

        const payload =
            await parseResponse(response);

        if (!response.ok) {
            const errorObject =
                payload?.error ?? {};

            const message =
                errorObject.message ??
                payload?.message ??
                `Ошибка HTTP ${response.status}`;

            throw new ApiError(
                message,
                {
                    status: response.status,
                    code:
                        errorObject.code ??
                        "http_error",
                    payload,
                }
            );
        }

        return payload;
    }

    const appApi = {
        ApiError,

        getHealth() {
            return request(
                "/api/health"
            );
        },

        getApplicationState() {
            return request(
                "/api/application/state"
            );
        },

        getChatHistory() {
            return request(
                "/api/chat/history"
            );
        },

        sendChatMessage(
            message,
            {
                signal,
            } = {}
        ) {
            return request(
                "/api/chat/messages",
                {
                    method: "POST",

                    body: JSON.stringify({
                        message,
                    }),

                    signal,
                }
            );
        },

        stopGeneration() {
            return request(
                "/api/chat/stop",
                {
                    method: "POST",
                }
            );
        },

        restartModelServer() {
            return request(
                "/api/server/restart",
                {
                    method: "POST",
                }
            );
        },

        clearChatHistory() {
            return request(
                "/api/chat/history",
                {
                    method: "DELETE",
                }
            );
        },

        activate({
                     pointName,
                     pointAddress,
                     activationKey,
                 }) {
            return request(
                "/api/auth/activate",
                {
                    method: "POST",

                    body: JSON.stringify({
                        point_name: pointName,
                        point_address: pointAddress,
                        activation_key: activationKey,
                    }),
                }
            );
        },

        submitReport(description) {
            return request(
                "/api/report",
                {
                    method: "POST",

                    body: JSON.stringify({
                        description,
                    }),
                }
            );
        },
    };

    window.appApi =
        Object.freeze(appApi);
})();