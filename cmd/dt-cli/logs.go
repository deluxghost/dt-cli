package main

import (
	"bufio"
	"context"
	"errors"
	"fmt"
	"os"
	"time"

	"dt-cli/internal/clientpipe"

	"github.com/urfave/cli/v3"
)

const logsPollInterval = 100 * time.Millisecond

type logsRequest struct {
	ID      string `json:"id"`
	Command string `json:"command"`
	Session string `json:"session,omitempty"`
}

type logsResponse struct {
	ID      string   `json:"id"`
	OK      bool     `json:"ok"`
	Session string   `json:"session,omitempty"`
	Entries []string `json:"entries,omitempty"`
	Dropped int      `json:"dropped,omitempty"`
	Error   string   `json:"error,omitempty"`
}

func newLogsCommand() *cli.Command {
	return &cli.Command{
		Name:      "logs",
		Usage:     "Stream new Darktide logs.",
		UsageText: "dt-cli logs",
		Action: func(ctx context.Context, command *cli.Command) error {
			if len(command.Args().Slice()) > 0 {
				return errors.New("logs does not accept arguments")
			}

			return runLogs(ctx)
		},
	}
}

func runLogs(parentCtx context.Context) error {
	session, err := startLogsSession(parentCtx)
	if err != nil {
		return err
	}

	defer stopLogsSession(session)

	writer := bufio.NewWriter(os.Stdout)
	defer writer.Flush()

	for {
		if err := parentCtx.Err(); err != nil {
			return err
		}

		response, err := pollLogsSession(parentCtx, session)
		if err != nil {
			return err
		}

		if response.Dropped > 0 {
			fmt.Fprintf(os.Stderr, "dt-cli logs: dropped %d log lines\n", response.Dropped)
		}

		for _, entry := range response.Entries {
			if _, err := fmt.Fprintln(writer, entry); err != nil {
				return err
			}
		}

		if err := writer.Flush(); err != nil {
			return err
		}

		if len(response.Entries) == 0 {
			time.Sleep(logsPollInterval)
		}
	}
}

func startLogsSession(parentCtx context.Context) (string, error) {
	request := logsRequest{
		ID:      newRequestID(),
		Command: "logs_start",
	}

	response, err := exchangeLogs(parentCtx, request)
	if err != nil {
		return "", err
	}
	if response.Session == "" {
		return "", errors.New("logs_start response did not include a session")
	}

	return response.Session, nil
}

func pollLogsSession(parentCtx context.Context, session string) (logsResponse, error) {
	request := logsRequest{
		ID:      newRequestID(),
		Command: "logs_poll",
		Session: session,
	}

	return exchangeLogs(parentCtx, request)
}

func stopLogsSession(session string) {
	ctx, cancel := context.WithTimeout(context.Background(), defaultTimeout)
	defer cancel()

	_, _ = exchangeLogs(ctx, logsRequest{
		ID:      newRequestID(),
		Command: "logs_stop",
		Session: session,
	})
}

func exchangeLogs(parentCtx context.Context, request logsRequest) (logsResponse, error) {
	ctx, cancel := context.WithTimeout(parentCtx, defaultTimeout)
	defer cancel()

	var response logsResponse
	if _, err := clientpipe.ExchangeJSON(ctx, request, &response); err != nil {
		return logsResponse{}, err
	}
	if err := clientpipe.ValidateResponseID(response.ID, request.ID); err != nil {
		return logsResponse{}, err
	}
	if !response.OK {
		if response.Error == "" {
			response.Error = "logs request failed"
		}
		return logsResponse{}, errors.New(response.Error)
	}

	return response, nil
}
