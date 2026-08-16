package ui

import "github.com/charmbracelet/lipgloss"

// Adaptive colors so the TUI stays readable on light and dark terminals.
var (
	colAccent = lipgloss.AdaptiveColor{Light: "#0550ae", Dark: "#7aa2f7"}
	colDim    = lipgloss.AdaptiveColor{Light: "#6e7781", Dark: "#7d8590"}
	colOK     = lipgloss.AdaptiveColor{Light: "#1a7f37", Dark: "#3fb950"}
	colErr    = lipgloss.AdaptiveColor{Light: "#cf222e", Dark: "#f85149"}
	colText   = lipgloss.AdaptiveColor{Light: "#1f2328", Dark: "#e6edf3"}
)

var (
	styleTitle = lipgloss.NewStyle().Bold(true).Foreground(colAccent)
	styleLabel = lipgloss.NewStyle().Foreground(colText)
	styleDim   = lipgloss.NewStyle().Foreground(colDim)
	styleFocus = lipgloss.NewStyle().Bold(true).Foreground(colAccent)
	styleBar   = lipgloss.NewStyle().Foreground(colAccent)
	styleOK    = lipgloss.NewStyle().Foreground(colOK)
	styleErr   = lipgloss.NewStyle().Foreground(colErr)

	styleSelected = lipgloss.NewStyle().Bold(true).
			Foreground(lipgloss.Color("#ffffff")).Background(colAccent).Padding(0, 1)

	stylePanel = lipgloss.NewStyle().
			Border(lipgloss.RoundedBorder()).BorderForeground(colDim).
			Padding(0, 1).MarginBottom(1)

	styleHelp = lipgloss.NewStyle().Foreground(colDim).MarginTop(1)
)
