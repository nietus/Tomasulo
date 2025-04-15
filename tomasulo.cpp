// Simulador do Algoritmo de Tomasulo

#include <iostream>
#include <fstream>
#include <vector>
#include <string>
#include <sstream>
#include <map>
#include <queue>
#include <iomanip>
#include <limits>
#include <cstdlib>

using namespace std;

// Definicao de tipos de instrucoes suportadas
enum InstructionType
{
    ADD,
    SUB,
    MUL,
    DIV,
    LOAD,
    STORE,
    INVALID
};

// Estrutura para representar uma instrucao
struct Instruction
{
    InstructionType type;
    string dest;     // Registrador de destino
    string src1;     // Primeiro operando fonte
    string src2;     // Segundo operando fonte
    int issue;       // Ciclo em que a instrucao foi emitida
    int execComp;    // Ciclo em que a execucao foi concluida
    int writeResult; // Ciclo em que o resultado foi escrito

    Instruction() : issue(-1), execComp(-1), writeResult(-1) {}
};

// Estrutura para representar uma estacao de reserva
struct ReservationStation
{
    bool busy;
    InstructionType op;
    int Vj, Vk;           // Valores dos operandos
    string Qj, Qk;        // Estacoes que produzirao os operandos (vazias se valores ja disponiveis)
    string dest;          // Registrador de destino
    int A;                // Usado para calcular enderecos em LOAD/STORE
    int instructionIndex; // indice da instrucao associada

    ReservationStation() : busy(false), op(INVALID), Vj(0), Vk(0), A(0), instructionIndex(-1) {}
};

// Estrutura para representar o status de um registrador
struct RegisterStatus
{
    bool busy;
    string reorderName; // Nome da estacao de reserva que produzira o resultado

    RegisterStatus() : busy(false) {}
};

// Classe principal do simulador
class TomasuloSimulator
{
public:
    // Verifica se a simulacao esta concluida
    bool isSimulationComplete() const
    {
        for (const auto &inst : instructions)
        {
            if (inst.writeResult == -1)
            {
                return false;
            }
        }
        return true;
    }

    // Avanca um ciclo da simulacao
    void stepSimulation()
    {
        // 1. Tentar emitir uma instrucao
        issueInstruction();

        // 2. Iniciar execucao de instrucoes prontas
        startExecution();

        // 3. Avancar a execucao das instrucoes em andamento
        advanceExecution();

        // 4. Processar resultados no CDB (Write-Back)
        processWriteBack();

        // Avancar para o próximo ciclo
        cycle++;
    }

    // Getter para o ciclo atual
    int getCurrentCycle() const
    {
        return cycle;
    }

    void printRegisters() const
    {
        cout << "\nValores Finais dos Registradores:\n";
        cout << "---------------------------------\n";
        for (const auto &reg : registers)
        {
            cout << reg.first << " = " << reg.second << endl;
        }
        cout << "---------------------------------\n";
    }

private:
    vector<Instruction> instructions;
    vector<ReservationStation> addRS;   // Estacoes de ADD/SUB
    vector<ReservationStation> mulRS;   // Estacoes de MUL/DIV
    vector<ReservationStation> loadRS;  // Estacoes de LOAD
    vector<ReservationStation> storeRS; // Estacoes de STORE

    map<string, int> registers;            // Registradores de dados (F0, F1, etc.)
    map<string, RegisterStatus> regStatus; // Status dos registradores

    int memory[1024];         // Memória simulada
    int cycle;                // Ciclo atual
    int nextInstructionIndex; // Próxima instrucao a ser emitida

    // Latências das operacoes (em ciclos)
    const int ADD_LATENCY = 2;
    const int MUL_LATENCY = 10;
    const int DIV_LATENCY = 40;
    const int LOAD_LATENCY = 2;
    const int STORE_LATENCY = 2;

    // Estruturas para controlar as execucoes em andamento
    struct ExecutingInstruction
    {
        int rsIndex;
        string rsType;
        int remainingCycles;
        int instructionIndex;
    };

    vector<ExecutingInstruction> executingInstructions;
    queue<int> completedInstructions; // Fila de instrucoes completadas aguardando CDB

public:
    TomasuloSimulator(int addRSCount = 3, int mulRSCount = 2, int loadRSCount = 3, int storeRSCount = 3)
    {
        // Inicializa estacoes de reserva
        addRS.resize(addRSCount);
        mulRS.resize(mulRSCount);
        loadRS.resize(loadRSCount);
        storeRS.resize(storeRSCount);

        // Inicializa registradores (F0-F31)
        for (int i = 0; i < 32; i++)
        {
            string regName = "F" + to_string(i);
            registers[regName] = 10;
            regStatus[regName] = RegisterStatus();
        }

        // Inicializa memória
        for (int i = 0; i < 1024; i++)
        {
            memory[i] = i; // Valor inicial é o próprio endereco, para facilitar teste
        }

        cycle = 0;
        nextInstructionIndex = 0;
    }

    // Carrega instrucoes de um arquivo
    bool loadInstructions(const string &filename)
    {
        ifstream file(filename);
        if (!file.is_open())
        {
            cerr << "Erro ao abrir arquivo: " << filename << endl;
            return false;
        }

        string line;
        while (getline(file, line))
        {
            // Ignora linhas vazias ou comentarios
            if (line.empty() || line[0] == '#')
            {
                continue;
            }

            istringstream iss(line);
            string op, dest, src1, src2;

            iss >> op >> dest;

            // Remover virgula do destino se houver
            if (!dest.empty() && dest.back() == ',')
            {
                dest.pop_back();
            }

            Instruction inst;

            // Determina o tipo de instrucao
            if (op == "ADD")
            {
                inst.type = ADD;
                iss >> src1 >> src2;
                // Remover virgula do src1 se houver
                if (!src1.empty() && src1.back() == ',')
                {
                    src1.pop_back();
                }
            }
            else if (op == "SUB")
            {
                inst.type = SUB;
                iss >> src1 >> src2;
                if (!src1.empty() && src1.back() == ',')
                {
                    src1.pop_back();
                }
            }
            else if (op == "MUL")
            {
                inst.type = MUL;
                iss >> src1 >> src2;
                if (!src1.empty() && src1.back() == ',')
                {
                    src1.pop_back();
                }
            }
            else if (op == "DIV")
            {
                inst.type = DIV;
                iss >> src1 >> src2;
                if (!src1.empty() && src1.back() == ',')
                {
                    src1.pop_back();
                }
            }
            else if (op == "L.D" || op == "LOAD")
            {
                inst.type = LOAD;
                string memRef;
                iss >> memRef;
                // Memória no formato offset(registrador)
                size_t openParen = memRef.find('(');
                size_t closeParen = memRef.find(')');
                if (openParen != string::npos && closeParen != string::npos)
                {
                    src1 = memRef.substr(0, openParen);                              // Offset
                    src2 = memRef.substr(openParen + 1, closeParen - openParen - 1); // Registrador base
                }
            }
            else if (op == "S.D" || op == "STORE")
            {
                inst.type = STORE;
                string memRef;
                iss >> memRef;
                src1 = dest; // Fonte é o registrador que queremos armazenar
                // Memória no formato offset(registrador)
                size_t openParen = memRef.find('(');
                size_t closeParen = memRef.find(')');
                if (openParen != string::npos && closeParen != string::npos)
                {
                    dest = memRef.substr(0, openParen);                              // Offset
                    src2 = memRef.substr(openParen + 1, closeParen - openParen - 1); // Registrador base
                }
            }
            else
            {
                cerr << "Instrucao nao reconhecida: " << op << endl;
                continue;
            }

            inst.dest = dest;
            inst.src1 = src1;
            inst.src2 = src2;

            instructions.push_back(inst);
        }

        file.close();
        return true;
    }

    // Encontra uma estacao de reserva livre para o tipo de instrucao
    pair<int, string> findFreeRS(InstructionType type)
    {
        if (type == ADD || type == SUB)
        {
            for (size_t i = 0; i < addRS.size(); i++)
            {
                if (!addRS[i].busy)
                {
                    return {i, "ADD"};
                }
            }
        }
        else if (type == MUL || type == DIV)
        {
            for (size_t i = 0; i < mulRS.size(); i++)
            {
                if (!mulRS[i].busy)
                {
                    return {i, "MUL"};
                }
            }
        }
        else if (type == LOAD)
        {
            for (size_t i = 0; i < loadRS.size(); i++)
            {
                if (!loadRS[i].busy)
                {
                    return {i, "LOAD"};
                }
            }
        }
        else if (type == STORE)
        {
            for (size_t i = 0; i < storeRS.size(); i++)
            {
                if (!storeRS[i].busy)
                {
                    return {i, "STORE"};
                }
            }
        }

        return {-1, ""};
    }

    // Emite uma instrucao para uma estacao de reserva
    bool issueInstruction()
    {
        if (nextInstructionIndex >= instructions.size())
        {
            return false;
        }

        Instruction &inst = instructions[nextInstructionIndex];
        std::pair<int, std::string> result = findFreeRS(inst.type);
        int rsIndex = result.first;
        std::string rsType = result.second;

        if (rsIndex == -1)
        {
            return false; // Nenhuma estacao de reserva disponivel
        }

        // Marca a instrucao como emitida no ciclo atual
        inst.issue = cycle;

        ReservationStation *rs = nullptr;

        if (rsType == "ADD")
        {
            rs = &addRS[rsIndex];
        }
        else if (rsType == "MUL")
        {
            rs = &mulRS[rsIndex];
        }
        else if (rsType == "LOAD")
        {
            rs = &loadRS[rsIndex];
        }
        else if (rsType == "STORE")
        {
            rs = &storeRS[rsIndex];
        }

        if (rs == nullptr)
        {
            return false;
        }

        // Configura a estacao de reserva
        rs->busy = true;
        rs->op = inst.type;
        rs->dest = inst.dest;
        rs->instructionIndex = nextInstructionIndex;

        // Obtém valores/status para os operandos
        if (inst.type == LOAD)
        {
            // Para LOAD: A = offset
            rs->A = stoi(inst.src1);
            rs->Qj = ""; // Nao depende de outros registradores
            rs->Qk = "";
        }
        else if (inst.type == STORE)
        {
            // Para STORE: A = offset, Vj/Qj = valor a ser armazenado
            rs->A = stoi(inst.dest);

            if (regStatus[inst.src1].busy)
            {
                rs->Qj = regStatus[inst.src1].reorderName;
                rs->Qk = "";
            }
            else
            {
                rs->Vj = registers[inst.src1];
                rs->Qj = "";
                rs->Qk = "";
            }
        }
        else
        {
            // Para operacoes aritméticas
            if (regStatus[inst.src1].busy)
            {
                rs->Qj = regStatus[inst.src1].reorderName;
            }
            else
            {
                rs->Vj = registers[inst.src1];
                rs->Qj = "";
            }

            if (regStatus[inst.src2].busy)
            {
                rs->Qk = regStatus[inst.src2].reorderName;
            }
            else
            {
                rs->Vk = registers[inst.src2];
                rs->Qk = "";
            }
        }

        // Atualiza o status do registrador de destino (exceto para STORE)
        if (inst.type != STORE)
        {
            regStatus[inst.dest].busy = true;
            regStatus[inst.dest].reorderName = rsType + to_string(rsIndex);
        }

        nextInstructionIndex++;
        return true;
    }

    // Verifica estacoes prontas para execucao e inicia execucao
    void startExecution()
    {
        // Verifica ADD/SUB
        for (size_t i = 0; i < addRS.size(); i++)
        {
            if (addRS[i].busy && addRS[i].Qj.empty() && addRS[i].Qk.empty())
            {
                bool alreadyExecuting = false;

                // Verifica se ja esta executando
                for (const auto &exec : executingInstructions)
                {
                    if (exec.rsType == "ADD" && exec.rsIndex == static_cast<int>(i))
                    {
                        alreadyExecuting = true;
                        break;
                    }
                }

                if (!alreadyExecuting)
                {
                    ExecutingInstruction exec;
                    exec.rsIndex = i;
                    exec.rsType = "ADD";
                    exec.instructionIndex = addRS[i].instructionIndex;

                    // Define latência baseada no tipo de operacao
                    if (addRS[i].op == ADD || addRS[i].op == SUB)
                    {
                        exec.remainingCycles = ADD_LATENCY;
                    }

                    executingInstructions.push_back(exec);
                }
            }
        }

        // Verifica MUL/DIV
        for (size_t i = 0; i < mulRS.size(); i++)
        {
            if (mulRS[i].busy && mulRS[i].Qj.empty() && mulRS[i].Qk.empty())
            {
                bool alreadyExecuting = false;

                // Verifica se ja esta executando
                for (const auto &exec : executingInstructions)
                {
                    if (exec.rsType == "MUL" && exec.rsIndex == static_cast<int>(i))
                    {
                        alreadyExecuting = true;
                        break;
                    }
                }

                if (!alreadyExecuting)
                {
                    ExecutingInstruction exec;
                    exec.rsIndex = i;
                    exec.rsType = "MUL";
                    exec.instructionIndex = mulRS[i].instructionIndex;

                    // Define latência baseada no tipo de operacao
                    if (mulRS[i].op == MUL)
                    {
                        exec.remainingCycles = MUL_LATENCY;
                    }
                    else if (mulRS[i].op == DIV)
                    {
                        exec.remainingCycles = DIV_LATENCY;
                    }

                    executingInstructions.push_back(exec);
                }
            }
        }

        // Verifica LOAD
        for (size_t i = 0; i < loadRS.size(); i++)
        {
            if (loadRS[i].busy && loadRS[i].Qj.empty() && loadRS[i].Qk.empty())
            {
                bool alreadyExecuting = false;

                // Verifica se ja esta executando
                for (const auto &exec : executingInstructions)
                {
                    if (exec.rsType == "LOAD" && exec.rsIndex == static_cast<int>(i))
                    {
                        alreadyExecuting = true;
                        break;
                    }
                }

                if (!alreadyExecuting)
                {
                    ExecutingInstruction exec;
                    exec.rsIndex = i;
                    exec.rsType = "LOAD";
                    exec.instructionIndex = loadRS[i].instructionIndex;
                    exec.remainingCycles = LOAD_LATENCY;

                    executingInstructions.push_back(exec);
                }
            }
        }

        // Verifica STORE
        for (size_t i = 0; i < storeRS.size(); i++)
        {
            if (storeRS[i].busy && storeRS[i].Qj.empty() && storeRS[i].Qk.empty())
            {
                bool alreadyExecuting = false;

                // Verifica se ja esta executando
                for (const auto &exec : executingInstructions)
                {
                    if (exec.rsType == "STORE" && exec.rsIndex == static_cast<int>(i))
                    {
                        alreadyExecuting = true;
                        break;
                    }
                }

                if (!alreadyExecuting)
                {
                    ExecutingInstruction exec;
                    exec.rsIndex = i;
                    exec.rsType = "STORE";
                    exec.instructionIndex = storeRS[i].instructionIndex;
                    exec.remainingCycles = STORE_LATENCY;

                    executingInstructions.push_back(exec);
                }
            }
        }
    }

    // Avanca a execucao das instrucoes em andamento
    void advanceExecution()
    {
        for (auto it = executingInstructions.begin(); it != executingInstructions.end();)
        {
            it->remainingCycles--;

            if (it->remainingCycles <= 0)
            {
                // Execucao concluida, marca o ciclo de conclusao
                instructions[it->instructionIndex].execComp = cycle;

                // Adiciona à fila de instrucoes completas (aguardando CDB)
                completedInstructions.push(it->instructionIndex);

                // Remove da lista de instrucoes em execucao
                it = executingInstructions.erase(it);
            }
            else
            {
                ++it;
            }
        }
    }

    // Processa o Common Data Bus (CDB) e escreve os resultados
    void processWriteBack()
    {
        if (completedInstructions.empty())
        {
            return;
        }

        int instIndex = completedInstructions.front();
        completedInstructions.pop();

        Instruction &inst = instructions[instIndex];
        inst.writeResult = cycle;

        // Encontra a estacao de reserva correspondente
        ReservationStation *rs = nullptr;
        string rsName;

        for (size_t i = 0; i < addRS.size(); i++)
        {
            if (addRS[i].busy && addRS[i].instructionIndex == instIndex)
            {
                rs = &addRS[i];
                rsName = "ADD" + to_string(i);
                break;
            }
        }

        if (rs == nullptr)
        {
            for (size_t i = 0; i < mulRS.size(); i++)
            {
                if (mulRS[i].busy && mulRS[i].instructionIndex == instIndex)
                {
                    rs = &mulRS[i];
                    rsName = "MUL" + to_string(i);
                    break;
                }
            }
        }

        if (rs == nullptr)
        {
            for (size_t i = 0; i < loadRS.size(); i++)
            {
                if (loadRS[i].busy && loadRS[i].instructionIndex == instIndex)
                {
                    rs = &loadRS[i];
                    rsName = "LOAD" + to_string(i);
                    break;
                }
            }
        }

        if (rs == nullptr)
        {
            for (size_t i = 0; i < storeRS.size(); i++)
            {
                if (storeRS[i].busy && storeRS[i].instructionIndex == instIndex)
                {
                    rs = &storeRS[i];
                    rsName = "STORE" + to_string(i);
                    break;
                }
            }
        }

        if (rs == nullptr)
        {
            cerr << "Erro: Nao foi possivel encontrar a estacao de reserva para a instrucao " << instIndex << endl;
            return;
        }

        // Calcula o resultado
        int result = 0;

        switch (inst.type)
        {
        case ADD:
            result = rs->Vj + rs->Vk;
            break;
        case SUB:
            result = rs->Vj - rs->Vk;
            break;
        case MUL:
            result = rs->Vj * rs->Vk;
            break;
        case DIV:
            if (rs->Vk != 0)
            {
                result = rs->Vj / rs->Vk;
            }
            else
            {
                cerr << "Erro: Divisao por zero!" << endl;
                result = 0;
            }
            break;
        case LOAD:
            result = memory[rs->A];
            break;
        case STORE:
            memory[rs->A] = rs->Vj;
            break;
        default:
            break;
        }

        // Atualiza registradores para todas as instrucoes exceto STORE
        if (inst.type != STORE)
        {
            registers[rs->dest] = result;

            // Atualiza o status do registrador
            if (regStatus[rs->dest].reorderName == rsName)
            {
                regStatus[rs->dest].busy = false;
            }
        }

        // Atualiza todas as estacoes de reserva que estavam esperando por este resultado
        updateDependentRS(rsName, result);

        // Libera a estacao de reserva
        rs->busy = false;
    }

    // Atualiza estacoes de reserva que dependem de um resultado
    void updateDependentRS(const string &rsName, int result)
    {
        // Atualiza ADD/SUB RS
        for (auto &rs : addRS)
        {
            if (rs.busy)
            {
                if (rs.Qj == rsName)
                {
                    rs.Vj = result;
                    rs.Qj = "";
                }
                if (rs.Qk == rsName)
                {
                    rs.Vk = result;
                    rs.Qk = "";
                }
            }
        }

        // Atualiza MUL/DIV RS
        for (auto &rs : mulRS)
        {
            if (rs.busy)
            {
                if (rs.Qj == rsName)
                {
                    rs.Vj = result;
                    rs.Qj = "";
                }
                if (rs.Qk == rsName)
                {
                    rs.Vk = result;
                    rs.Qk = "";
                }
            }
        }

        // Atualiza LOAD RS
        for (auto &rs : loadRS)
        {
            if (rs.busy)
            {
                if (rs.Qj == rsName)
                {
                    rs.Vj = result;
                    rs.Qj = "";
                }
                if (rs.Qk == rsName)
                {
                    rs.Vk = result;
                    rs.Qk = "";
                }
            }
        }

        // Atualiza STORE RS
        for (auto &rs : storeRS)
        {
            if (rs.busy)
            {
                if (rs.Qj == rsName)
                {
                    rs.Vj = result;
                    rs.Qj = "";
                }
                if (rs.Qk == rsName)
                {
                    rs.Vk = result;
                    rs.Qk = "";
                }
            }
        }
    }

    // Imprime o estado atual da simulacao
    void printStatus()
    {
        cout << "\n";
        cout << "==== Ciclo " << cycle << " ====" << endl;

        // Imprime as instrucoes e seu status
        cout << "\nInstrucoes:" << endl;
        cout << "-----------------------------------------------------------------" << endl;
        cout << "| # | Instrucao        | Emissao | Exec Comp | Write Result |" << endl;
        cout << "-----------------------------------------------------------------" << endl;

        for (size_t i = 0; i < instructions.size(); i++)
        {
            const Instruction &inst = instructions[i];
            string instStr;

            switch (inst.type)
            {
            case ADD:
                instStr = "ADD " + inst.dest + "," + inst.src1 + "," + inst.src2;
                break;
            case SUB:
                instStr = "SUB " + inst.dest + "," + inst.src1 + "," + inst.src2;
                break;
            case MUL:
                instStr = "MUL " + inst.dest + "," + inst.src1 + "," + inst.src2;
                break;
            case DIV:
                instStr = "DIV " + inst.dest + "," + inst.src1 + "," + inst.src2;
                break;
            case LOAD:
                instStr = "LOAD " + inst.dest + "," + inst.src1 + "(" + inst.src2 + ")";
                break;
            case STORE:
                instStr = "STORE " + inst.src1 + "," + inst.dest + "(" + inst.src2 + ")";
                break;
            default:
                instStr = "INVALID";
                break;
            }

            cout << "| " << setw(2) << i << " | " << left << setw(16) << instStr << " | ";
            cout << setw(7) << (inst.issue != -1 ? to_string(inst.issue) : "-") << " | ";
            cout << setw(9) << (inst.execComp != -1 ? to_string(inst.execComp) : "-") << " | ";
            cout << setw(12) << (inst.writeResult != -1 ? to_string(inst.writeResult) : "-") << " |" << endl;
        }
        cout << "-----------------------------------------------------------------" << endl;

        // Imprime estacoes de reserva ADD/SUB
        cout << "\nEstacoes de Reserva ADD/SUB:" << endl;
        cout << "--------------------------------------------------------------------" << endl;
        cout << "| # | Busy | Op  | Vj  | Vk  | Qj  | Qk  | Dest | A   | InstIdx |" << endl;
        cout << "--------------------------------------------------------------------" << endl;

        for (size_t i = 0; i < addRS.size(); i++)
        {
            const ReservationStation &rs = addRS[i];
            string opStr;

            if (rs.busy)
            {
                switch (rs.op)
                {
                case ADD:
                    opStr = "ADD";
                    break;
                case SUB:
                    opStr = "SUB";
                    break;
                default:
                    opStr = "???";
                    break;
                }
            }
            else
            {
                opStr = "";
            }

            cout << "| " << setw(1) << i << " | " << setw(4) << (rs.busy ? "Sim" : "Nao") << " | " << setw(3) << opStr << " | ";
            cout << setw(3) << (rs.busy && rs.Qj.empty() ? to_string(rs.Vj) : "-") << " | ";
            cout << setw(3) << (rs.busy && rs.Qk.empty() ? to_string(rs.Vk) : "-") << " | ";
            cout << setw(3) << (rs.busy ? rs.Qj : "-") << " | ";
            cout << setw(3) << (rs.busy ? rs.Qk : "-") << " | ";
            cout << setw(4) << (rs.busy ? rs.dest : "-") << " | ";
            cout << setw(3) << (rs.busy ? to_string(rs.A) : "-") << " | ";
            cout << setw(7) << (rs.busy ? to_string(rs.instructionIndex) : "-") << " |" << endl;
        }
        cout << "--------------------------------------------------------------------" << endl;

        // Imprime estacoes de reserva MUL/DIV
        cout << "\nEstacoes de Reserva MUL/DIV:" << endl;
        cout << "--------------------------------------------------------------------" << endl;
        cout << "| # | Busy | Op  | Vj  | Vk  | Qj  | Qk  | Dest | A   | InstIdx |" << endl;
        cout << "--------------------------------------------------------------------" << endl;

        for (size_t i = 0; i < mulRS.size(); i++)
        {
            const ReservationStation &rs = mulRS[i];
            string opStr;

            if (rs.busy)
            {
                switch (rs.op)
                {
                case MUL:
                    opStr = "MUL";
                    break;
                case DIV:
                    opStr = "DIV";
                    break;
                default:
                    opStr = "???";
                    break;
                }
            }
            else
            {
                opStr = "";
            }

            cout << "| " << setw(1) << i << " | " << setw(4) << (rs.busy ? "Sim" : "Nao") << " | " << setw(3) << opStr << " | ";
            cout << setw(3) << (rs.busy && rs.Qj.empty() ? to_string(rs.Vj) : "-") << " | ";
            cout << setw(3) << (rs.busy && rs.Qk.empty() ? to_string(rs.Vk) : "-") << " | ";
            cout << setw(3) << (rs.busy ? rs.Qj : "-") << " | ";
            cout << setw(3) << (rs.busy ? rs.Qk : "-") << " | ";
            cout << setw(4) << (rs.busy ? rs.dest : "-") << " | ";
            cout << setw(3) << (rs.busy ? to_string(rs.A) : "-") << " | ";
            cout << setw(7) << (rs.busy ? to_string(rs.instructionIndex) : "-") << " |" << endl;
        }
        cout << "--------------------------------------------------------------------" << endl;
    }
};

int main()
{
    TomasuloSimulator simulator;

    // Carregar instrucoes de um arquivo
    string filename;
    cout << "Digite o nome do arquivo de instrucoes: ";
    cin >> filename;

    if (!simulator.loadInstructions(filename))
    {
        cerr << "Falha ao carregar instrucoes. Finalizando." << endl;
        return 1;
    }

    while (!simulator.isSimulationComplete())
    {
        // Imprimir status atual
        simulator.printStatus();

        // Executar um ciclo da simulacao
        simulator.stepSimulation();

        // Aguardar enter para avancar um ciclo
        cout << "\nAvancar [ENTER]";
        cin.get();
    }

    // Imprimir status final
    cout << "\n=== Simulacao concluida ===" << endl;
    simulator.printStatus();
    simulator.printRegisters();

    return 0;
}