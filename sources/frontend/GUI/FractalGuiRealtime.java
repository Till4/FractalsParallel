import javax.swing.*;
import java.awt.*;
import java.awt.event.*;
import java.awt.image.BufferedImage;
import java.io.*;

public class FractalGuiRealtime extends JFrame {

    private volatile boolean restartPending = false;

    private JComboBox<String> backendSelector;
    private JButton startButton;
    private JButton stopButton;
    private JButton resetButton;
    private JSpinner widthSpinner;
    private JSpinner heightSpinner;
    private JLabel imageLabel;

    private volatile boolean running = false;
    private volatile double zoom = 1.0;
    private volatile double centerX = 0.0, centerY = 0.0;

    // Default image size
    private int WIDTH = 800, HEIGHT = 600;
    private byte[] buffer = new byte[WIDTH * HEIGHT * 3]; // RGB bytes per frame
    private int frameSize;
    private final double MOVE_STEP = 0.1;
    private final double ZOOM_FACTOR = 0.9;

    // Store the last mouse position for drag operations
    private int lastMouseX;
    private int lastMouseY;

    // Define the initial world dimensions for the fractal at zoom 1.0
    // Assuming a typical Mandelbrot range like x:[-2, 2], y:[-1.5, 1.5]
    private final double INITIAL_WORLD_WIDTH = 4.0;
    private final double INITIAL_WORLD_HEIGHT = 3.0;

    private Process externalProcess = null;
    private OutputStream processStdin;
    private InputStream processStdout;

    // --- Debounce-Variablen für gesteuerte Aktualisierungen ---
    // paramSendTimer wird nur noch für Tastatur-Schwenken verwendet
    private Timer paramSendTimer;
    private Timer dragUpdateTimer; // Timer für das kontinuierliche Senden während des Ziehens
    private final int DEBOUNCE_DELAY_MS = 150; // Verzögerung für Tastatur-Schwenken
    private final int DRAG_UPDATE_DELAY_MS = 50; // Update-Rate während des Ziehens

    public FractalGuiRealtime() {
        super("Fractal Live Renderer");

        backendSelector = new JComboBox<>(new String[] {
                "CUDA",
                "Rust",
                "C MPI",
                "C OpenMP"

        });

        startButton = new JButton("Start");
        stopButton = new JButton("Stop");
        resetButton = new JButton("Reset");

        // Initialisierung des Timers für Tastatur-Schwenken
        paramSendTimer = new Timer(DEBOUNCE_DELAY_MS, new ActionListener() {
            @Override
            public void actionPerformed(ActionEvent e) {
                // Dieser Code wird ausgeführt, wenn der Timer abläuft
                sendParameters();
                paramSendTimer.stop(); // Timer stoppen, bis er erneut ausgelöst wird
            }
        });
        paramSendTimer.setRepeats(false); // Der Timer soll nur einmal auslösen

        // Initialisierung des Timers für Drag-Updates
        dragUpdateTimer = new Timer(DRAG_UPDATE_DELAY_MS, new ActionListener() {
            @Override
            public void actionPerformed(ActionEvent e) {
                // Dieser Code wird während des Ziehens periodisch ausgeführt
                sendParameters();
            }
        });
        dragUpdateTimer.setRepeats(true); // Dieser Timer wiederholt sich, solange gezogen wird

        startButton.addActionListener(e -> {
            if (!running) {
                running = true;
                backendSelector.setEnabled(false);
                startRenderLoop();
            }
        });
        stopButton.addActionListener(e -> {
            running = false;
            // Sicherstellen, dass alle Timer gestoppt werden, wenn die Wiedergabe beendet
            // wird
            paramSendTimer.stop();
            dragUpdateTimer.stop();
            backendSelector.setEnabled(true);
            if (externalProcess != null) {
                externalProcess.destroy();
                externalProcess = null;
            }
        });
        resetButton.addActionListener(e -> {
            if (running) {
                resetView();
            }
        });

        widthSpinner = new JSpinner(new SpinnerNumberModel(WIDTH, 100, 4000, 10));
        heightSpinner = new JSpinner(new SpinnerNumberModel(HEIGHT, 100, 4000, 10));

        widthSpinner.addChangeListener(e -> updateResolutionFromUI());
        heightSpinner.addChangeListener(e -> updateResolutionFromUI());

        JPanel topPanel = new JPanel();
        topPanel.add(new JLabel("Backend:"));
        topPanel.add(backendSelector);
        topPanel.add(startButton);
        topPanel.add(stopButton);
        topPanel.add(resetButton);

        topPanel.add(new JLabel("Width:"));
        topPanel.add(widthSpinner);
        topPanel.add(new JLabel("Height:"));
        topPanel.add(heightSpinner);

        imageLabel = new JLabel();
        imageLabel.setPreferredSize(new Dimension(WIDTH, HEIGHT));
        imageLabel.setOpaque(true);
        imageLabel.setBackground(Color.BLACK);

        add(topPanel, BorderLayout.NORTH);
        add(imageLabel, BorderLayout.CENTER);

        setupMouseBindings();

        setFocusable(true);
        setDefaultCloseOperation(EXIT_ON_CLOSE);
        pack();
        setVisible(true);
    }

    private void updateResolutionFromUI() {
        int newWidth = (int) widthSpinner.getValue();
        int newHeight = (int) heightSpinner.getValue();

        if (newWidth != WIDTH || newHeight != HEIGHT) {
            WIDTH = newWidth;
            HEIGHT = newHeight;

            imageLabel.setPreferredSize(new Dimension(WIDTH, HEIGHT));
            pack(); // GUI-Größe anpassen

            if (running) {
                // Wenn das Rendering läuft, stoßen wir einen sauberen Neustart an.
                System.out.println("Resolution changed. Signalling for restart...");
                restartPending = true; // Signal setzen, dass wir neustarten wollen
                running = false; // Signal an die while-Schleife, sich zu beenden

                // Prozess beenden. Dies unterbricht auch den blockierenden read() im
                // Render-Thread
                if (externalProcess != null) {
                    externalProcess.destroy();
                }
            }
        }
    }

    private void setupMouseBindings() {
        // Mouse Wheel Listener for Zoom
        imageLabel.addMouseWheelListener(e -> {
            if (running) {
                int notches = e.getWheelRotation();
                if (notches < 0) { // Wheel moved up (zoom in)
                    zoom /= ZOOM_FACTOR;
                } else { // Wheel moved down (zoom out)
                    zoom *= ZOOM_FACTOR;
                }
                sendParameters(); // Direkt senden für Zoom
            }
        });

        // Mouse Listener for Click and Drag Panning
        imageLabel.addMouseListener(new MouseAdapter() {
            @Override
            public void mousePressed(MouseEvent e) {
                if (running && SwingUtilities.isLeftMouseButton(e)) {
                    lastMouseX = e.getX();
                    lastMouseY = e.getY();
                    dragUpdateTimer.start(); // Startet den Timer für kontinuierliche Updates beim Ziehen
                }
            }

            @Override
            public void mouseReleased(MouseEvent e) {
                if (running && SwingUtilities.isLeftMouseButton(e)) {
                    dragUpdateTimer.stop(); // Stoppt den kontinuierlichen Update-Timer
                    sendParameters(); // Sende ein finales Bild für die genaue Position
                }
            }
        });

        imageLabel.addMouseMotionListener(new MouseMotionAdapter() {
            @Override
            public void mouseDragged(MouseEvent e) {
                if (running && SwingUtilities.isLeftMouseButton(e)) {
                    int currentMouseX = e.getX();
                    int currentMouseY = e.getY();

                    int deltaPx = currentMouseX - lastMouseX;
                    int deltaPy = currentMouseY - lastMouseY;

                    double currentWorldWidth = INITIAL_WORLD_WIDTH / zoom;
                    double currentWorldHeight = INITIAL_WORLD_HEIGHT / zoom;

                    centerX -= (double) deltaPx * (currentWorldWidth / WIDTH);
                    centerY += (double) deltaPy * (currentWorldHeight / HEIGHT);

                    lastMouseX = currentMouseX;
                    lastMouseY = currentMouseY;

                    // Der dragUpdateTimer kümmert sich um das Senden der Parameter
                    // Kein direkter sendParameters() Aufruf hier
                }
            }
        });
    }

    private void resetView() {
        zoom = 1.0;
        centerX = 0.0;
        centerY = 0.0;
        sendParameters();
    }

    private void startRenderLoop() {
        new Thread(() -> {
            frameSize = WIDTH * HEIGHT * 3;
            buffer = new byte[frameSize];
            try {
                String backend = (String) backendSelector.getSelectedItem();
                ProcessBuilder pb = getProcessBuilderForBackend(backend);
                externalProcess = pb.start();
                System.out.println("Backend-Prozess gestartet: " + backend);

                // stderr correctly read:
                new Thread(() -> {
                    try (BufferedReader err = new BufferedReader(
                            new InputStreamReader(externalProcess.getErrorStream()))) {
                        String line;
                        while ((line = err.readLine()) != null) {
                            System.err.println("[Backend STDERR] " + line);
                        }
                    } catch (IOException e) {
                        e.printStackTrace();
                    }
                }).start();

                processStdin = externalProcess.getOutputStream();
                processStdout = externalProcess.getInputStream();
                sendParameters(); // Initiales Bild anfordern

                // Die Haupt-Render-Schleife
                while (running) {
                    int bytesRead = 0;
                    while (bytesRead < frameSize) {
                        int r = processStdout.read(buffer, bytesRead, frameSize - bytesRead);
                        if (r == -1) {
                            if (!running)
                                break; // Geplanter Stopp, kein Fehler
                            throw new IOException("Process closed stream unexpectedly");
                        }
                        bytesRead += r;
                    }
                    if (!running)
                        break; // Überprüfen, ob nach dem Lesen ein Stopp signalisiert wurde

                    BufferedImage img = bytesToBufferedImage(buffer, WIDTH, HEIGHT);
                    SwingUtilities.invokeLater(() -> imageLabel.setIcon(new ImageIcon(img)));
                }

            } catch (IOException ex) {
                // Diese Exception wird erwartet, wenn wir .destroy() aufrufen.
                if (restartPending) {
                    System.out.println("Render-Loop stopped for restart.");
                } else {
                    // Ein unerwarteter Fehler
                    System.err.println("Render-Loop IO-Error: " + ex.getMessage());
                }
            } catch (Exception ex) {
                // Alle anderen Fehler
                ex.printStackTrace();
                // Hier kein showMessageDialog, um die UI nicht zu blockieren, wenn ein Neustart
                // ansteht
            } finally {
                // **SAUBERES AUFRÄUMEN UND NEUSTART**
                if (externalProcess != null) {
                    externalProcess.destroy();
                    externalProcess = null;
                }

                // Prüfen, ob ein Neustart angefordert wurde
                if (restartPending) {
                    restartPending = false; // Flag zurücksetzen
                    // Den Start-Vorgang sicher auf dem UI-Thread auslösen
                    SwingUtilities.invokeLater(() -> {
                        System.out.println("Restarting render process...");
                        startButton.doClick(); // Jetzt ist es sicher, den Start-Button zu klicken
                    });
                } else {
                    // Wenn kein Neustart, dann ist der Stopp-Vorgang abgeschlossen. UI-Elemente
                    // wieder aktivieren.
                    SwingUtilities.invokeLater(() -> backendSelector.setEnabled(true));
                }
            }
        }).start();
    }

    private void sendParameters() {
        if (processStdin == null)
            return;
        try {
            String msg = zoom + " " + centerX + " " + centerY + " " + WIDTH + " " + HEIGHT + "\n";
            processStdin.write(msg.getBytes());
            processStdin.flush();
            System.out.println("Parameter gesendet: Zoom=" + zoom + ", X=" + centerX + ", Y=" + centerY
                    + ", Width=" + WIDTH + ", Height=" + HEIGHT);
        } catch (IOException e) {
            e.printStackTrace();
            System.err.println("Error sending parameters to backend: " + e.getMessage());
        }
    }

    private BufferedImage bytesToBufferedImage(byte[] bytes, int width, int height) {
        BufferedImage img = new BufferedImage(width, height, BufferedImage.TYPE_INT_RGB);
        int idx = 0;
        for (int y = 0; y < height; y++) {
            for (int x = 0; x < width; x++) {
                int r = bytes[idx++] & 0xFF;
                int g = bytes[idx++] & 0xFF;
                int b = bytes[idx++] & 0xFF;
                int rgb = (r << 16) | (g << 8) | b;
                img.setRGB(x, y, rgb);
            }
        }
        return img;
    }

    private ProcessBuilder getProcessBuilderForBackend(String backend) {
        switch (backend) {

            case "CUDA":
                String os = System.getProperty("os.name").toLowerCase();

                if (os.contains("win")) {
                    // Windows
                    return new ProcessBuilder("bin/backend/cuda/CudaFractalBackend.exe");
                } else if (os.contains("nix") || os.contains("nux") || os.contains("mac")) {
                    // Linux
                    return new ProcessBuilder("bin/backend/cuda/CudaFractalBackend");
                } else {
                    throw new UnsupportedOperationException("Unsupported OS for CUDA backend: " + os);
                }
      
            case "Rust":
                return new ProcessBuilder("./fractal_rust");
            case "C MPI":
                return new ProcessBuilder(
                        "sources/Backend/c/fractal_c_mpi_live");
            case "C OpenMP":
                return new ProcessBuilder(
                        "sources/Backend/c/fractal_c_openmp_live");
            default:
                throw new IllegalArgumentException("Unbekanntes Backend: " + backend);
        }
    }

    public static void main(String[] args) {
        SwingUtilities.invokeLater(FractalGuiRealtime::new);
    }
}
